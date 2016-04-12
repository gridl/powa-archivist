/*-------------------------------------------------------------------------
 *
 * powa.c: PoWA background worker
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/* For a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* Access a database */
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"

/* Some catalog elements */
#include "catalog/pg_type.h"
#include "utils/timestamp.h"

/* There is a GUC */
#include "utils/guc.h"

/* We use tuplestore */
#include "funcapi.h"

/* pgsats access */
#include "pgstat.h"

PG_MODULE_MAGIC;

#define POWA_STAT_FUNC_COLS 4 /* # of cols for functions stat SRF */
#define POWA_STAT_TAB_COLS 21 /* # of cols for relations stat SRF */

typedef enum
{
	POWA_STAT_FUNCTION,
	POWA_STAT_TABLE
} PowaStatKind;

void        _PG_init(void);
void        die_on_too_small_frequency(void);

Datum			powa_stat_user_functions(PG_FUNCTION_ARGS);
Datum			powa_stat_all_rel(PG_FUNCTION_ARGS);
static Datum	powa_stat_common(PG_FUNCTION_ARGS, PowaStatKind kind);

PG_FUNCTION_INFO_V1(powa_stat_user_functions);
PG_FUNCTION_INFO_V1(powa_stat_all_rel);

static void powa_main(Datum main_arg);
static void powa_sighup(SIGNAL_ARGS);

static int  powa_frequency;
static int  min_powa_frequency = 5000;
static int  powa_retention;
static int  powa_coalesce;
static char *powa_database = NULL;
static char *powa_ignored_users = NULL;

void die_on_too_small_frequency(void)
{
    if (powa_frequency > 0 && powa_frequency < min_powa_frequency)
      {
          elog(LOG, "POWA frequency cannot be smaller than %i milliseconds",
               min_powa_frequency);
          exit(1);
      }
}

void _PG_init(void)
{

    BackgroundWorker worker;

	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "This module can only be loaded via shared_preload_libraries");
		return;
	}


    DefineCustomIntVariable("powa.frequency",
                            "Defines the frequency in seconds of the snapshots",
                            NULL,
                            &powa_frequency,
                            300000,
                            -1,
                            INT_MAX / 1000,
                            PGC_SUSET, GUC_UNIT_MS, NULL, NULL, NULL);

    DefineCustomIntVariable("powa.coalesce",
                            "Defines the amount of records to group together in the table (more compact)",
                            NULL,
                            &powa_coalesce,
                            100, 5, INT_MAX, PGC_SUSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("powa.retention",
                            "Automatically purge data older than N minutes",
                            NULL,
                            &powa_retention,
                            HOURS_PER_DAY * MINS_PER_HOUR,
                            0,
                            INT_MAX / SECS_PER_MINUTE,
                            PGC_SUSET, GUC_UNIT_MIN, NULL, NULL, NULL);

    DefineCustomStringVariable("powa.database",
                               "Defines the database of the workload repository",
                               NULL,
                               &powa_database,
                               "powa", PGC_POSTMASTER, 0, NULL, NULL, NULL);

    DefineCustomStringVariable("powa.ignored_users",
                               "Defines a coma-separated list of users to ignore when taking activity snapshot",
                               NULL,
                               &powa_ignored_users,
                               NULL, PGC_SIGHUP, 0, NULL, NULL, NULL);
    /*
       Register the worker processes
     */
    worker.bgw_flags =
        BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;     /* Must write to the database */
    worker.bgw_main = powa_main;
    snprintf(worker.bgw_name, BGW_MAXLEN, "powa");
    worker.bgw_restart_time = 10;
    worker.bgw_main_arg = (Datum) 0;
#if (PG_VERSION_NUM >= 90400)
    worker.bgw_notify_pid = 0;
#endif
    RegisterBackgroundWorker(&worker);
}


static void powa_main(Datum main_arg)
{
    char		   *query_snapshot = "SELECT powa_take_snapshot()";
    static char	   *query_appname = "SET application_name = 'POWA collector'";
    instr_time		begin;
    instr_time		end;
    long			time_to_wait;

    die_on_too_small_frequency();
    /*
       Set up signal handler, then unblock signals
     */
    pqsignal(SIGHUP, powa_sighup);

    BackgroundWorkerUnblockSignals();

    /*
       We only connect when powa_frequency >0. If not, powa has been deactivated
     */
    if (powa_frequency < 0)
    {
        elog(LOG, "POWA is deactivated (powa.frequency = %i), exiting",
             powa_frequency);
        exit(1);
    }
    /* We got here: it means powa_frequency > 0. Let's connect */


    /*
       Connect to POWA database
     */
    BackgroundWorkerInitializeConnection(powa_database, NULL);

    elog(LOG, "POWA connected to database %s", quote_identifier(powa_database));

    StartTransactionCommand();
    SetCurrentStatementStartTimestamp();
    SPI_connect();
    PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, query_appname);
    SPI_execute(query_appname, false, 0);
    SPI_finish();
    PopActiveSnapshot();
    CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, NULL);

    /*------------------
	 * Main loop of POWA
	 * We exit from here if:
	 *   - we got a SIGINT/SIGTERM (default bgworker sig handlers)
	 *   - powa.frequency becomes < 0 (change config and SIGHUP)
	 */
	while (true)
    {
        /*
           We can get here with a new value of powa_frequency
           because of a reload. Let's suicide to disconnect
           if this value is <0
         */
        if (powa_frequency < 0)
        {
            elog(LOG, "POWA exits to disconnect from the database now");
            exit(1);
        }

        /*
           let's store the current time. It will be used to
           calculate a quite stable interval between each measure
         */
        INSTR_TIME_SET_CURRENT(begin);
        ResetLatch(&MyProc->procLatch);
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        SPI_connect();
        PushActiveSnapshot(GetTransactionSnapshot());
		pgstat_report_activity(STATE_RUNNING, query_snapshot);
        SPI_execute(query_snapshot, false, 0);
        SPI_finish();
        PopActiveSnapshot();
        CommitTransactionCommand();
		pgstat_report_activity(STATE_IDLE, NULL);
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, begin);
        /*
           Wait powa.frequency, compensate for work time of last snapshot
         */
        /*
           If we got off schedule (because of a compact or delete,
           just do another operation right now
         */
        time_to_wait = powa_frequency - INSTR_TIME_GET_MILLISEC(end);
        elog(DEBUG1, "Waiting for %li milliseconds", time_to_wait);
        if (time_to_wait > 0)
        {
			StringInfoData buf;

			initStringInfo(&buf);

			appendStringInfo(&buf, "-- sleeping for %li seconds",
					time_to_wait / 1000);

			pgstat_report_activity(STATE_IDLE, buf.data);

			pfree(buf.data);

            WaitLatch(&MyProc->procLatch,
                      WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                      time_to_wait);
        }
    }
}


static void powa_sighup(SIGNAL_ARGS)
{
    ProcessConfigFile(PGC_SIGHUP);
    die_on_too_small_frequency();
}

Datum
powa_stat_user_functions(PG_FUNCTION_ARGS)
{
	return powa_stat_common(fcinfo, POWA_STAT_FUNCTION);
}

Datum
powa_stat_all_rel(PG_FUNCTION_ARGS)
{
	return powa_stat_common(fcinfo, POWA_STAT_TABLE);
}

static Datum	powa_stat_common(PG_FUNCTION_ARGS, PowaStatKind kind)
{
	Oid			dbid = PG_GETARG_OID(0);
	Oid			backend_dbid;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	PgStat_StatDBEntry *dbentry;
	HASH_SEQ_STATUS hash_seq;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* -----------------------------------------------------
	 * Force deep statistics retrieval of specified database.
	 *
	 * Deep means to also include tables and functions HTAB, which is what we
	 * want here.
	 *
	 * The stat collector isn't suppose to act this way, since a backend can't
	 * access data outside the database it's connected to.  It's not a problem
	 * here since we only need the identifier that are stored in the pgstats,
	 * the UI will connect to the database to do the lookup.
	 *
	 * So, to ensure we'll have fresh statitics of the wanted database, we have
	 * to do following (ugly) tricks:
	 *
	 * - clear the current statistics cache. If a previous function already
	 *   asked for statistics in the same transaction, calling
	 *   pgstat_fetch_stat_dbentry would() just return the cache, which would
	 *   probably belong to another database. As the powa snapshot works inside
	 *   a function, we have the guarantee that this function will be called for
	 *   all the databases in a single transaction anyway.
	 *
	 * - change the global var MyDatabaseId to the wanted databaseid. pgstat
	 *   is designed to only retrieve statistics for current database, so we
	 *   need to fool it.
	 *
	 * - call pgstat_fetch_stat_dbentry().
	 *
	 * - restore MyDatabaseId.
	 *
	 * - and finally clear again the statistics cache, to make sure any further
	 *   statement in the transaction will see the data related to the right
	 *   database.
	 */

	pgstat_clear_snapshot();

	backend_dbid = MyDatabaseId;
	MyDatabaseId = dbid;

	dbentry = pgstat_fetch_stat_dbentry(dbid);

	MyDatabaseId = backend_dbid;

	if (dbentry != NULL && dbentry->functions != NULL)
	{
		switch (kind)
		{
			case POWA_STAT_FUNCTION:
			{
				PgStat_StatFuncEntry *funcentry = NULL;

				hash_seq_init(&hash_seq, dbentry->functions);
				while ((funcentry = hash_seq_search(&hash_seq)) != NULL)
				{
					Datum		values[POWA_STAT_FUNC_COLS];
					bool		nulls[POWA_STAT_FUNC_COLS];
					int			i = 0;

					memset(values, 0, sizeof(values));
					memset(nulls, 0, sizeof(nulls));

					values[i++] = ObjectIdGetDatum(funcentry->functionid);
					values[i++] = Int64GetDatum(funcentry->f_numcalls);
					values[i++] = Float8GetDatum(((double) funcentry->f_total_time) / 1000.0);
					values[i++] = Float8GetDatum(((double) funcentry->f_self_time) / 1000.0);

					Assert(i == POWA_STAT_FUNC_COLS);

					tuplestore_putvalues(tupstore, tupdesc, values, nulls);
				}
				break;
			}
			case POWA_STAT_TABLE:
			{
				PgStat_StatTabEntry *tabentry = NULL;

				hash_seq_init(&hash_seq, dbentry->tables);
				while ((tabentry = hash_seq_search(&hash_seq)) != NULL)
				{
					Datum		values[POWA_STAT_TAB_COLS];
					bool		nulls[POWA_STAT_TAB_COLS];
					int			i = 0;

					memset(values, 0, sizeof(values));
					memset(nulls, 0, sizeof(nulls));

					/* Oid of the table (or index) */
					values[i++] = ObjectIdGetDatum(tabentry->tableid);

					values[i++] = Int64GetDatum((int64) tabentry->numscans);

					values[i++] = Int64GetDatum((int64) tabentry->tuples_returned);
					values[i++] = Int64GetDatum((int64) tabentry->tuples_fetched);
					values[i++] = Int64GetDatum((int64) tabentry->tuples_inserted);
					values[i++] = Int64GetDatum((int64) tabentry->tuples_updated);
					values[i++] = Int64GetDatum((int64) tabentry->tuples_deleted);
					values[i++] = Int64GetDatum((int64) tabentry->tuples_hot_updated);

					values[i++] = Int64GetDatum((int64) tabentry->n_live_tuples);
					values[i++] = Int64GetDatum((int64) tabentry->n_dead_tuples);
					values[i++] = Int64GetDatum((int64) tabentry->changes_since_analyze);

					values[i++] = Int64GetDatum((int64) (tabentry->blocks_fetched - tabentry->blocks_hit));
					values[i++] = Int64GetDatum((int64) tabentry->blocks_hit);

					/* last vacuum */
					if (tabentry->vacuum_timestamp == 0)
						nulls[i++] = true;
					else
						values[i++] = TimestampTzGetDatum(tabentry->vacuum_timestamp);
					values[i++] = Int64GetDatum((int64) tabentry->vacuum_count);

					/* last_autovacuum */
					if (tabentry->autovac_vacuum_timestamp == 0)
						nulls[i++] = true;
					else
						values[i++] = TimestampTzGetDatum(tabentry->autovac_vacuum_timestamp);
					values[i++] = Int64GetDatum((int64) tabentry->autovac_vacuum_count);

					/* last_analyze */
					if (tabentry->analyze_timestamp == 0)
						nulls[i++] = true;
					else
						values[i++] = TimestampTzGetDatum(tabentry->analyze_timestamp);
					values[i++] = Int64GetDatum((int64) tabentry->analyze_count);

					/* last_autoanalyze */
					if (tabentry->autovac_analyze_timestamp == 0)
						nulls[i++] = true;
					else
						values[i++] = TimestampTzGetDatum(tabentry->autovac_analyze_timestamp);
					values[i++] = Int64GetDatum((int64) tabentry->autovac_analyze_count);

					Assert(i == POWA_STAT_TAB_COLS);

					tuplestore_putvalues(tupstore, tupdesc, values, nulls);
				}
				break;
			}
		}
	}

	/*
	 * Make sure any subsequent statistic retrieving will not see the one we
	 * just fetched
	 */
	pgstat_clear_snapshot();


	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

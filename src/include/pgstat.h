/* ----------
 *	pgstat.h
 *
 *	Definitions for the PostgreSQL cumulative statistics system.
 *
 *	Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 *	src/include/pgstat.h
 * ----------
 */
#ifndef PGSTAT_H
#define PGSTAT_H

#include "datatype/timestamp.h"
#include "portability/instr_time.h"
#include "postmaster/pgarch.h"	/* for MAX_XFN_CHARS */
#include "utils/backend_progress.h" /* for backward compatibility */
#include "utils/backend_status.h"	/* for backward compatibility */
#include "utils/relcache.h"
#include "utils/wait_event.h"	/* for backward compatibility */

#include "postmaster/autostats.h"


/* ----------
 * Paths for the statistics files (relative to installation's $PGDATA).
 * ----------
 */
#define PGSTAT_STAT_PERMANENT_DIRECTORY		"pg_stat"
#define PGSTAT_STAT_PERMANENT_FILENAME		"pg_stat/pgstat.stat"
#define PGSTAT_STAT_PERMANENT_TMPFILE		"pg_stat/pgstat.tmp"

/* Default directory to store temporary statistics data in */
#define PG_STAT_TMP_DIR		"pg_stat_tmp"

/* The types of statistics entries */
typedef enum PgStat_Kind
{
	/* use 0 for INVALID, to catch zero-initialized data */
	PGSTAT_KIND_INVALID = 0,

	/* stats for variable-numbered objects */
	PGSTAT_KIND_DATABASE,		/* database-wide statistics */
	PGSTAT_KIND_RELATION,		/* per-table statistics */
	PGSTAT_KIND_FUNCTION,		/* per-function statistics */
	PGSTAT_KIND_REPLSLOT,		/* per-slot statistics */
	PGSTAT_KIND_SUBSCRIPTION,	/* per-subscription statistics */

	/* stats for fixed-numbered objects */
	PGSTAT_KIND_ARCHIVER,
	PGSTAT_KIND_BGWRITER,
	PGSTAT_KIND_CHECKPOINTER,
	PGSTAT_KIND_IO,
	PGSTAT_KIND_SLRU,
	PGSTAT_KIND_WAL,
} PgStat_Kind;

#define PGSTAT_KIND_FIRST_VALID PGSTAT_KIND_DATABASE
#define PGSTAT_KIND_LAST PGSTAT_KIND_WAL
#define PGSTAT_NUM_KINDS (PGSTAT_KIND_LAST + 1)

/* Values for track_functions GUC variable --- order is significant! */
typedef enum TrackFunctionsLevel
{
	TRACK_FUNC_OFF,
	TRACK_FUNC_PL,
	TRACK_FUNC_ALL
}			TrackFunctionsLevel;

typedef enum PgStat_FetchConsistency
{
	PGSTAT_FETCH_CONSISTENCY_NONE,
	PGSTAT_FETCH_CONSISTENCY_CACHE,
	PGSTAT_FETCH_CONSISTENCY_SNAPSHOT,
} PgStat_FetchConsistency;

/* Values to track the cause of session termination */
typedef enum SessionEndType
{
	DISCONNECT_NOT_YET,			/* still active */
	DISCONNECT_NORMAL,
	DISCONNECT_CLIENT_EOF,
	DISCONNECT_FATAL,
	DISCONNECT_KILLED
} SessionEndType;

/* ----------
<<<<<<< HEAD
 * The types of backend -> collector messages
 * ----------
 */
typedef enum StatMsgType
{
	PGSTAT_MTYPE_DUMMY,
	PGSTAT_MTYPE_INQUIRY,
	PGSTAT_MTYPE_TABSTAT,
	PGSTAT_MTYPE_TABPURGE,
	PGSTAT_MTYPE_DROPDB,
	PGSTAT_MTYPE_RESETCOUNTER,
	PGSTAT_MTYPE_RESETSHAREDCOUNTER,
	PGSTAT_MTYPE_RESETSINGLECOUNTER,
	PGSTAT_MTYPE_RESETSLRUCOUNTER,
	PGSTAT_MTYPE_RESETREPLSLOTCOUNTER,
	PGSTAT_MTYPE_AUTOVAC_START,
	PGSTAT_MTYPE_VACUUM,
	PGSTAT_MTYPE_ANALYZE,
	PGSTAT_MTYPE_ARCHIVER,
	PGSTAT_MTYPE_QUEUESTAT, /* GPDB */
	PGSTAT_MTYPE_BGWRITER,
	PGSTAT_MTYPE_WAL,
	PGSTAT_MTYPE_SLRU,
	PGSTAT_MTYPE_FUNCSTAT,
	PGSTAT_MTYPE_FUNCPURGE,
	PGSTAT_MTYPE_RECOVERYCONFLICT,
	PGSTAT_MTYPE_TEMPFILE,
	PGSTAT_MTYPE_DEADLOCK,
	PGSTAT_MTYPE_CHECKSUMFAILURE,
	PGSTAT_MTYPE_REPLSLOT,
	PGSTAT_MTYPE_CONNECT,
	PGSTAT_MTYPE_DISCONNECT,
} StatMsgType;

/* ----------
=======
>>>>>>> REL_16_9
 * The data type used for counters.
 * ----------
 */
typedef int64 PgStat_Counter;


/* ------------------------------------------------------------
 * Structures kept in backend local memory while accumulating counts
 * ------------------------------------------------------------
 */

/* ----------
 * PgStat_FunctionCounts	The actual per-function counts kept by a backend
 *
 * This struct should contain only actual event counters, because we memcmp
 * it against zeroes to detect whether there are any pending stats.
 *
 * Note that the time counters are in instr_time format here.  We convert to
 * microseconds in PgStat_Counter format when flushing out pending statistics.
 * ----------
 */
typedef struct PgStat_FunctionCounts
{
	PgStat_Counter numcalls;
	instr_time	total_time;
	instr_time	self_time;
} PgStat_FunctionCounts;

/*
 * Working state needed to accumulate per-function-call timing statistics.
 */
typedef struct PgStat_FunctionCallUsage
{
	/* Link to function's hashtable entry (must still be there at exit!) */
	/* NULL means we are not tracking the current function call */
	PgStat_FunctionCounts *fs;
	/* Total time previously charged to function, as of function start */
	instr_time	save_f_total_time;
	/* Backend-wide total time as of function start */
	instr_time	save_total;
	/* system clock as of function start */
	instr_time	start;
} PgStat_FunctionCallUsage;

/* ----------
 * PgStat_BackendSubEntry	Non-flushed subscription stats.
 * ----------
 */
typedef struct PgStat_BackendSubEntry
{
	PgStat_Counter apply_error_count;
	PgStat_Counter sync_error_count;
} PgStat_BackendSubEntry;

/* ----------
 * PgStat_TableCounts			The actual per-table counts kept by a backend
 *
 * This struct should contain only actual event counters, because we memcmp
 * it against zeroes to detect whether there are any stats updates to apply.
 * It is a component of PgStat_TableStatus (within-backend state).
 *
 * Note: for a table, tuples_returned is the number of tuples successfully
 * fetched by heap_getnext, while tuples_fetched is the number of tuples
 * successfully fetched by heap_fetch under the control of bitmap indexscans.
 * For an index, tuples_returned is the number of index entries returned by
 * the index AM, while tuples_fetched is the number of tuples successfully
 * fetched by heap_fetch under the control of simple indexscans for this index.
 *
<<<<<<< HEAD
 * tuples_inserted/updated/deleted/hot_updated count attempted actions,
 * regardless of whether the transaction committed.  delta_live_tuples,
=======
 * tuples_inserted/updated/deleted/hot_updated/newpage_updated count attempted
 * actions, regardless of whether the transaction committed.  delta_live_tuples,
>>>>>>> REL_16_9
 * delta_dead_tuples, and changed_tuples are set depending on commit or abort.
 * Note that delta_live_tuples and delta_dead_tuples can be negative!
 * ----------
 */
typedef struct PgStat_TableCounts
{
	PgStat_Counter numscans;

	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;

	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	PgStat_Counter tuples_hot_updated;
	PgStat_Counter tuples_newpage_updated;
	bool		truncdropped;

	PgStat_Counter delta_live_tuples;
	PgStat_Counter delta_dead_tuples;
	PgStat_Counter changed_tuples;

	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;
} PgStat_TableCounts;

/* ----------
 * PgStat_TableStatus			Per-table status within a backend
 *
 * Many of the event counters are nontransactional, ie, we count events
 * in committed and aborted transactions alike.  For these, we just count
 * directly in the PgStat_TableStatus.  However, delta_live_tuples,
 * delta_dead_tuples, and changed_tuples must be derived from event counts
 * with awareness of whether the transaction or subtransaction committed or
 * aborted.  Hence, we also keep a stack of per-(sub)transaction status
 * records for every table modified in the current transaction.  At commit
 * or abort, we propagate tuples_inserted/updated/deleted up to the
 * parent subtransaction level, or out to the parent PgStat_TableStatus,
 * as appropriate.
 * ----------
 */
typedef struct PgStat_TableStatus
{
	Oid			id;				/* table's OID */
	bool		shared;			/* is it a shared catalog? */
	struct PgStat_TableXactStatus *trans;	/* lowest subxact's counts */
	PgStat_TableCounts counts;	/* event counts to be sent */
	Relation	relation;		/* rel that is using this entry */
} PgStat_TableStatus;

/* ----------
 * PgStat_TableXactStatus		Per-table, per-subtransaction status
 * ----------
 */
typedef struct PgStat_TableXactStatus
{
	PgStat_Counter tuples_inserted; /* tuples inserted in (sub)xact */
	PgStat_Counter tuples_updated;	/* tuples updated in (sub)xact */
	PgStat_Counter tuples_deleted;	/* tuples deleted in (sub)xact */
	bool		truncdropped;	/* relation truncated/dropped in this
								 * (sub)xact */
	/* tuples i/u/d prior to truncate/drop */
	PgStat_Counter inserted_pre_truncdrop;
	PgStat_Counter updated_pre_truncdrop;
	PgStat_Counter deleted_pre_truncdrop;
	int			nest_level;		/* subtransaction nest level */
	/* links to other structs for same relation: */
	struct PgStat_TableXactStatus *upper;	/* next higher subxact if any */
	PgStat_TableStatus *parent; /* per-table status */
	/* structs of same subxact level are linked here: */
	struct PgStat_TableXactStatus *next;	/* next of same subxact */
} PgStat_TableXactStatus;


/* ------------------------------------------------------------
<<<<<<< HEAD
 * Message formats follow
 * ------------------------------------------------------------
 */


/* ----------
 * PgStat_MsgHdr				The common message header
 * ----------
 */
typedef struct PgStat_MsgHdr
{
	StatMsgType m_type;
	int			m_size;
} PgStat_MsgHdr;

/* ----------
 * Space available in a message.  This will keep the UDP packets below 1K,
 * which should fit unfragmented into the MTU of the loopback interface.
 * (Larger values of PGSTAT_MAX_MSG_SIZE would work for that on most
 * platforms, but we're being conservative here.)
 * ----------
 */
#define PGSTAT_MAX_MSG_SIZE 1000
#define PGSTAT_MSG_PAYLOAD	(PGSTAT_MAX_MSG_SIZE - sizeof(PgStat_MsgHdr))


/* ----------
 * PgStat_MsgDummy				A dummy message, ignored by the collector
 * ----------
 */
typedef struct PgStat_MsgDummy
{
	PgStat_MsgHdr m_hdr;
} PgStat_MsgDummy;


/* ----------
 * PgStat_MsgInquiry			Sent by a backend to ask the collector
 *								to write the stats file(s).
 *
 * Ordinarily, an inquiry message prompts writing of the global stats file,
 * the stats file for shared catalogs, and the stats file for the specified
 * database.  If databaseid is InvalidOid, only the first two are written.
 *
 * New file(s) will be written only if the existing file has a timestamp
 * older than the specified cutoff_time; this prevents duplicated effort
 * when multiple requests arrive at nearly the same time, assuming that
 * backends send requests with cutoff_times a little bit in the past.
 *
 * clock_time should be the requestor's current local time; the collector
 * uses this to check for the system clock going backward, but it has no
 * effect unless that occurs.  We assume clock_time >= cutoff_time, though.
 * ----------
 */

typedef struct PgStat_MsgInquiry
{
	PgStat_MsgHdr m_hdr;
	TimestampTz clock_time;		/* observed local clock time */
	TimestampTz cutoff_time;	/* minimum acceptable file timestamp */
	Oid			databaseid;		/* requested DB (InvalidOid => shared only) */
} PgStat_MsgInquiry;


/* ----------
 * PgStat_TableEntry			Per-table info in a MsgTabstat
 * ----------
 */
typedef struct PgStat_TableEntry
{
	Oid			t_id;
	PgStat_TableCounts t_counts;
} PgStat_TableEntry;

/* ----------
 * PgStat_MsgTabstat			Sent by the backend to report table
 *								and buffer access statistics.
 * ----------
 */
#define PGSTAT_NUM_TABENTRIES  \
	((PGSTAT_MSG_PAYLOAD - sizeof(Oid) - 3 * sizeof(int) - 5 * sizeof(PgStat_Counter)) \
	 / sizeof(PgStat_TableEntry))

typedef struct PgStat_MsgTabstat
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_nentries;
	int			m_xact_commit;
	int			m_xact_rollback;
	PgStat_Counter m_block_read_time;	/* times in microseconds */
	PgStat_Counter m_block_write_time;
	PgStat_Counter m_session_time;
	PgStat_Counter m_active_time;
	PgStat_Counter m_idle_in_xact_time;
	PgStat_TableEntry m_entry[PGSTAT_NUM_TABENTRIES];
} PgStat_MsgTabstat;


/* ----------
 * PgStat_MsgTabpurge			Sent by the backend to tell the collector
 *								about dead tables.
 * ----------
 */
#define PGSTAT_NUM_TABPURGE  \
	((PGSTAT_MSG_PAYLOAD - sizeof(Oid) - sizeof(int))  \
	 / sizeof(Oid))

typedef struct PgStat_MsgTabpurge
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_nentries;
	Oid			m_tableid[PGSTAT_NUM_TABPURGE];
} PgStat_MsgTabpurge;


/* ----------
 * PgStat_MsgDropdb				Sent by the backend to tell the collector
 *								about a dropped database
 * ----------
 */
typedef struct PgStat_MsgDropdb
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
} PgStat_MsgDropdb;


/* ----------
 * PgStat_MsgResetcounter		Sent by the backend to tell the collector
 *								to reset counters
 * ----------
 */
typedef struct PgStat_MsgResetcounter
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
} PgStat_MsgResetcounter;

/* ----------
 * PgStat_MsgResetsharedcounter Sent by the backend to tell the collector
 *								to reset a shared counter
 * ----------
 */
typedef struct PgStat_MsgResetsharedcounter
{
	PgStat_MsgHdr m_hdr;
	PgStat_Shared_Reset_Target m_resettarget;
} PgStat_MsgResetsharedcounter;

/* ----------
 * PgStat_MsgResetsinglecounter Sent by the backend to tell the collector
 *								to reset a single counter
 * ----------
 */
typedef struct PgStat_MsgResetsinglecounter
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	PgStat_Single_Reset_Type m_resettype;
	Oid			m_objectid;
} PgStat_MsgResetsinglecounter;

/* ----------
 * PgStat_MsgResetslrucounter Sent by the backend to tell the collector
 *								to reset a SLRU counter
 * ----------
 */
typedef struct PgStat_MsgResetslrucounter
{
	PgStat_MsgHdr m_hdr;
	int			m_index;
} PgStat_MsgResetslrucounter;

/* ----------
 * PgStat_MsgResetreplslotcounter Sent by the backend to tell the collector
 *								to reset replication slot counter(s)
 * ----------
 */
typedef struct PgStat_MsgResetreplslotcounter
{
	PgStat_MsgHdr m_hdr;
	NameData	m_slotname;
	bool		clearall;
} PgStat_MsgResetreplslotcounter;

/* ----------
 * PgStat_MsgAutovacStart		Sent by the autovacuum daemon to signal
 *								that a database is going to be processed
 * ----------
 */
typedef struct PgStat_MsgAutovacStart
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	TimestampTz m_start_time;
} PgStat_MsgAutovacStart;


/* ----------
 * PgStat_MsgVacuum				Sent by the backend or autovacuum daemon
 *								after VACUUM
 * ----------
 */
typedef struct PgStat_MsgVacuum
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	Oid			m_tableoid;
	bool		m_autovacuum;
	TimestampTz m_vacuumtime;
	PgStat_Counter m_live_tuples;
	PgStat_Counter m_dead_tuples;
} PgStat_MsgVacuum;


/* ----------
 * PgStat_MsgAnalyze			Sent by the backend or autovacuum daemon
 *								after ANALYZE
 * ----------
 */
typedef struct PgStat_MsgAnalyze
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	Oid			m_tableoid;
	bool		m_autovacuum;
	bool		m_resetcounter;
	TimestampTz m_analyzetime;
	PgStat_Counter m_live_tuples;
	PgStat_Counter m_dead_tuples;
} PgStat_MsgAnalyze;


/* ----------
 * PgStat_MsgArchiver			Sent by the archiver to update statistics.
 * ----------
 */
typedef struct PgStat_MsgArchiver
{
	PgStat_MsgHdr m_hdr;
	bool		m_failed;		/* Failed attempt */
	char		m_xlog[MAX_XFN_CHARS + 1];
	TimestampTz m_timestamp;
} PgStat_MsgArchiver;

/* ----------
 * PgStat_MsgQueuestat			Sent by the backend to report resource queue
 *								activity statistics.
 * ----------  GPDB 
 */
typedef struct PgStat_MsgQueuestat
{
	PgStat_MsgHdr m_hdr;
	Oid			m_queueid;
	PgStat_Counter	m_queries_exec;
	PgStat_Counter	m_queries_wait;
	PgStat_Counter	m_elapsed_exec;
	PgStat_Counter	m_elapsed_wait;
} PgStat_MsgQueuestat;

/* ----------
 * PgStat_MsgBgWriter			Sent by the bgwriter to update statistics.
 * ----------
 */
typedef struct PgStat_MsgBgWriter
{
	PgStat_MsgHdr m_hdr;

	PgStat_Counter m_timed_checkpoints;
	PgStat_Counter m_requested_checkpoints;
	PgStat_Counter m_buf_written_checkpoints;
	PgStat_Counter m_buf_written_clean;
	PgStat_Counter m_maxwritten_clean;
	PgStat_Counter m_buf_written_backend;
	PgStat_Counter m_buf_fsync_backend;
	PgStat_Counter m_buf_alloc;
	PgStat_Counter m_checkpoint_write_time; /* times in milliseconds */
	PgStat_Counter m_checkpoint_sync_time;
} PgStat_MsgBgWriter;

/* ----------
 * PgStat_MsgWal			Sent by backends and background processes to update WAL statistics.
 * ----------
 */
typedef struct PgStat_MsgWal
{
	PgStat_MsgHdr m_hdr;
	PgStat_Counter m_wal_records;
	PgStat_Counter m_wal_fpi;
	uint64		m_wal_bytes;
	PgStat_Counter m_wal_buffers_full;
	PgStat_Counter m_wal_write;
	PgStat_Counter m_wal_sync;
	PgStat_Counter m_wal_write_time;	/* time spent writing wal records in
										 * microseconds */
	PgStat_Counter m_wal_sync_time; /* time spent syncing wal records in
									 * microseconds */
} PgStat_MsgWal;

/* ----------
 * PgStat_MsgSLRU			Sent by a backend to update SLRU statistics.
 * ----------
 */
typedef struct PgStat_MsgSLRU
{
	PgStat_MsgHdr m_hdr;
	PgStat_Counter m_index;
	PgStat_Counter m_blocks_zeroed;
	PgStat_Counter m_blocks_hit;
	PgStat_Counter m_blocks_read;
	PgStat_Counter m_blocks_written;
	PgStat_Counter m_blocks_exists;
	PgStat_Counter m_flush;
	PgStat_Counter m_truncate;
} PgStat_MsgSLRU;

/* ----------
 * PgStat_MsgReplSlot	Sent by a backend or a wal sender to update replication
 *						slot statistics.
 * ----------
 */
typedef struct PgStat_MsgReplSlot
{
	PgStat_MsgHdr m_hdr;
	NameData	m_slotname;
	bool		m_create;
	bool		m_drop;
	PgStat_Counter m_spill_txns;
	PgStat_Counter m_spill_count;
	PgStat_Counter m_spill_bytes;
	PgStat_Counter m_stream_txns;
	PgStat_Counter m_stream_count;
	PgStat_Counter m_stream_bytes;
	PgStat_Counter m_total_txns;
	PgStat_Counter m_total_bytes;
} PgStat_MsgReplSlot;


/* ----------
 * PgStat_MsgRecoveryConflict	Sent by the backend upon recovery conflict
 * ----------
 */
typedef struct PgStat_MsgRecoveryConflict
{
	PgStat_MsgHdr m_hdr;

	Oid			m_databaseid;
	int			m_reason;
} PgStat_MsgRecoveryConflict;

/* ----------
 * PgStat_MsgTempFile	Sent by the backend upon creating a temp file
 * ----------
 */
typedef struct PgStat_MsgTempFile
{
	PgStat_MsgHdr m_hdr;

	Oid			m_databaseid;
	size_t		m_filesize;
} PgStat_MsgTempFile;

/* ----------
 * PgStat_FunctionCounts	The actual per-function counts kept by a backend
 *
 * This struct should contain only actual event counters, because we memcmp
 * it against zeroes to detect whether there are any counts to transmit.
 *
 * Note that the time counters are in instr_time format here.  We convert to
 * microseconds in PgStat_Counter format when transmitting to the collector.
 * ----------
 */
typedef struct PgStat_FunctionCounts
{
	PgStat_Counter f_numcalls;
	instr_time	f_total_time;
	instr_time	f_self_time;
} PgStat_FunctionCounts;

/* ----------
 * PgStat_BackendFunctionEntry	Entry in backend's per-function hash table
 * ----------
 */
typedef struct PgStat_BackendFunctionEntry
{
	Oid			f_id;
	PgStat_FunctionCounts f_counts;
} PgStat_BackendFunctionEntry;

/* ----------
 * PgStat_FunctionEntry			Per-function info in a MsgFuncstat
 * ----------
 */
typedef struct PgStat_FunctionEntry
{
	Oid			f_id;
	PgStat_Counter f_numcalls;
	PgStat_Counter f_total_time;	/* times in microseconds */
	PgStat_Counter f_self_time;
} PgStat_FunctionEntry;

/* ----------
 * PgStat_MsgFuncstat			Sent by the backend to report function
 *								usage statistics.
 * ----------
 */
#define PGSTAT_NUM_FUNCENTRIES	\
	((PGSTAT_MSG_PAYLOAD - sizeof(Oid) - sizeof(int))  \
	 / sizeof(PgStat_FunctionEntry))

typedef struct PgStat_MsgFuncstat
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_nentries;
	PgStat_FunctionEntry m_entry[PGSTAT_NUM_FUNCENTRIES];
} PgStat_MsgFuncstat;

/* ----------
 * PgStat_MsgFuncpurge			Sent by the backend to tell the collector
 *								about dead functions.
 * ----------
 */
#define PGSTAT_NUM_FUNCPURGE  \
	((PGSTAT_MSG_PAYLOAD - sizeof(Oid) - sizeof(int))  \
	 / sizeof(Oid))

typedef struct PgStat_MsgFuncpurge
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_nentries;
	Oid			m_functionid[PGSTAT_NUM_FUNCPURGE];
} PgStat_MsgFuncpurge;

/* ----------
 * PgStat_MsgDeadlock			Sent by the backend to tell the collector
 *								about a deadlock that occurred.
 * ----------
 */
typedef struct PgStat_MsgDeadlock
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
} PgStat_MsgDeadlock;

/* ----------
 * PgStat_MsgChecksumFailure	Sent by the backend to tell the collector
 *								about checksum failures noticed.
 * ----------
 */
typedef struct PgStat_MsgChecksumFailure
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_failurecount;
	TimestampTz m_failure_time;
} PgStat_MsgChecksumFailure;

/* ----------
 * PgStat_MsgConnect			Sent by the backend upon connection
 *								establishment
 * ----------
 */
typedef struct PgStat_MsgConnect
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
} PgStat_MsgConnect;

/* ----------
 * PgStat_MsgDisconnect			Sent by the backend when disconnecting
 * ----------
 */
typedef struct PgStat_MsgDisconnect
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	SessionEndType m_cause;
} PgStat_MsgDisconnect;

/* ----------
 * PgStat_Msg					Union over all possible messages.
 * ----------
 */
typedef union PgStat_Msg
{
	PgStat_MsgHdr msg_hdr;
	PgStat_MsgDummy msg_dummy;
	PgStat_MsgInquiry msg_inquiry;
	PgStat_MsgTabstat msg_tabstat;
	PgStat_MsgTabpurge msg_tabpurge;
	PgStat_MsgDropdb msg_dropdb;
	PgStat_MsgResetcounter msg_resetcounter;
	PgStat_MsgResetsharedcounter msg_resetsharedcounter;
	PgStat_MsgResetsinglecounter msg_resetsinglecounter;
	PgStat_MsgResetslrucounter msg_resetslrucounter;
	PgStat_MsgResetreplslotcounter msg_resetreplslotcounter;
	PgStat_MsgAutovacStart msg_autovacuum_start;
	PgStat_MsgVacuum msg_vacuum;
	PgStat_MsgAnalyze msg_analyze;
	PgStat_MsgArchiver msg_archiver;
	PgStat_MsgQueuestat msg_queuestat;  /* GPDB */
	PgStat_MsgBgWriter msg_bgwriter;
	PgStat_MsgWal msg_wal;
	PgStat_MsgSLRU msg_slru;
	PgStat_MsgFuncstat msg_funcstat;
	PgStat_MsgFuncpurge msg_funcpurge;
	PgStat_MsgRecoveryConflict msg_recoveryconflict;
	PgStat_MsgDeadlock msg_deadlock;
	PgStat_MsgTempFile msg_tempfile;
	PgStat_MsgChecksumFailure msg_checksumfailure;
	PgStat_MsgReplSlot msg_replslot;
	PgStat_MsgConnect msg_connect;
	PgStat_MsgDisconnect msg_disconnect;
} PgStat_Msg;


/* ------------------------------------------------------------
 * Statistic collector data structures follow
=======
 * Data structures on disk and in shared memory follow
>>>>>>> REL_16_9
 *
 * PGSTAT_FILE_FORMAT_ID should be changed whenever any of these
 * data structures change.
 * ------------------------------------------------------------
 */

#define PGSTAT_FILE_FORMAT_ID	0x01A5BCAC

<<<<<<< HEAD
/* ----------
 * PgStat_StatDBEntry			The collector's data per database
 * ----------
 */
typedef struct PgStat_StatDBEntry
{
	Oid			databaseid;
	PgStat_Counter n_xact_commit;
	PgStat_Counter n_xact_rollback;
	PgStat_Counter n_blocks_fetched;
	PgStat_Counter n_blocks_hit;
	PgStat_Counter n_tuples_returned;
	PgStat_Counter n_tuples_fetched;
	PgStat_Counter n_tuples_inserted;
	PgStat_Counter n_tuples_updated;
	PgStat_Counter n_tuples_deleted;
	TimestampTz last_autovac_time;
	PgStat_Counter n_conflict_tablespace;
	PgStat_Counter n_conflict_lock;
	PgStat_Counter n_conflict_snapshot;
	PgStat_Counter n_conflict_bufferpin;
	PgStat_Counter n_conflict_startup_deadlock;
	PgStat_Counter n_temp_files;
	PgStat_Counter n_temp_bytes;
	PgStat_Counter n_deadlocks;
	PgStat_Counter n_checksum_failures;
	TimestampTz last_checksum_failure;
	PgStat_Counter n_block_read_time;	/* times in microseconds */
	PgStat_Counter n_block_write_time;
	PgStat_Counter n_sessions;
	PgStat_Counter total_session_time;
	PgStat_Counter total_active_time;
	PgStat_Counter total_idle_in_xact_time;
	PgStat_Counter n_sessions_abandoned;
	PgStat_Counter n_sessions_fatal;
	PgStat_Counter n_sessions_killed;

	TimestampTz stat_reset_timestamp;
	TimestampTz stats_timestamp;	/* time of db stats file update */

	/*
	 * tables and functions must be last in the struct, because we don't write
	 * the pointers out to the stats file.
	 */
	HTAB	   *tables;
	HTAB	   *functions;
} PgStat_StatDBEntry;


/* ----------
 * PgStat_StatTabEntry			The collector's data per table (or index)
 * ----------
 */
typedef struct PgStat_StatTabEntry
{
	Oid			tableid;

	PgStat_Counter numscans;

	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;

	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	PgStat_Counter tuples_hot_updated;

	PgStat_Counter n_live_tuples;
	PgStat_Counter n_dead_tuples;
	PgStat_Counter changes_since_analyze;
	PgStat_Counter unused_counter;	/* kept for ABI compatibility */
	PgStat_Counter inserts_since_vacuum;

	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;

	TimestampTz vacuum_timestamp;	/* user initiated vacuum */
	PgStat_Counter vacuum_count;
	TimestampTz autovac_vacuum_timestamp;	/* autovacuum initiated */
	PgStat_Counter autovac_vacuum_count;
	TimestampTz analyze_timestamp;	/* user initiated */
	PgStat_Counter analyze_count;
	TimestampTz autovac_analyze_timestamp;	/* autovacuum initiated */
	PgStat_Counter autovac_analyze_count;
} PgStat_StatTabEntry;


/* ----------
 * PgStat_StatQueueEntry		The collector's data per resource queue
 * ----------
 *  --- cdb extension ---
 */
typedef struct PgStat_StatQueueEntry
{
	Oid			queueid;
	PgStat_Counter n_queries_exec;
	PgStat_Counter n_queries_wait;
	PgStat_Counter elapsed_exec;
	PgStat_Counter elapsed_wait;
} PgStat_StatQueueEntry;

/* ----------
 * PgStat_StatFuncEntry			The collector's data per function
 * ----------
 */
typedef struct PgStat_StatFuncEntry
{
	Oid			functionid;

	PgStat_Counter f_numcalls;

	PgStat_Counter f_total_time;	/* times in microseconds */
	PgStat_Counter f_self_time;
} PgStat_StatFuncEntry;



/* ----------
 * PgStat_StatPortalEntry
 *
 * Each backend maintains a hash that contains a PgStat_StatPortalEntry for
 * each portal. This is so we can easily track elapsed times for execute and
 * wait, as well as counts for each of these. 
 *
 * In order to (hopefully) not cause a performance degradation when queue
 * statistics are enabled, we only store and caclulate elapsed time to the 
 * nearest second (via the time() call). This may mean that large numbers of 
 * sub-second queries get ignored due to roundoff. 
 *
 * Note that the collector never sees these structs, the PgStat_StatQueueEntry
 * is what is sent at report time.
 * ----------
 */
typedef struct PgStat_StatPortalEntry
{
	uint32					portalid;
	time_t					t_exec_start;
	time_t					t_exec_end;
	time_t					t_wait_start;
	time_t					t_wait_end;
	PgStat_StatQueueEntry	queueentry;
} PgStat_StatPortalEntry;


/*
 * Archiver statistics kept in the stats collector
 */
=======
>>>>>>> REL_16_9
typedef struct PgStat_ArchiverStats
{
	PgStat_Counter archived_count;	/* archival successes */
	char		last_archived_wal[MAX_XFN_CHARS + 1];	/* last WAL file
														 * archived */
	TimestampTz last_archived_timestamp;	/* last archival success time */
	PgStat_Counter failed_count;	/* failed archival attempts */
	char		last_failed_wal[MAX_XFN_CHARS + 1]; /* WAL file involved in
													 * last failure */
	TimestampTz last_failed_timestamp;	/* last archival failure time */
	TimestampTz stat_reset_timestamp;
} PgStat_ArchiverStats;

typedef struct PgStat_BgWriterStats
{
	PgStat_Counter buf_written_clean;
	PgStat_Counter maxwritten_clean;
	PgStat_Counter buf_alloc;
	TimestampTz stat_reset_timestamp;
} PgStat_BgWriterStats;

typedef struct PgStat_CheckpointerStats
{
	PgStat_Counter timed_checkpoints;
	PgStat_Counter requested_checkpoints;
	PgStat_Counter checkpoint_write_time;	/* times in milliseconds */
	PgStat_Counter checkpoint_sync_time;
	PgStat_Counter buf_written_checkpoints;
	PgStat_Counter buf_written_backend;
	PgStat_Counter buf_fsync_backend;
} PgStat_CheckpointerStats;


/*
 * Types related to counting IO operations
 */
typedef enum IOObject
{
	IOOBJECT_RELATION,
	IOOBJECT_TEMP_RELATION,
} IOObject;

#define IOOBJECT_NUM_TYPES (IOOBJECT_TEMP_RELATION + 1)

typedef enum IOContext
{
	IOCONTEXT_BULKREAD,
	IOCONTEXT_BULKWRITE,
	IOCONTEXT_NORMAL,
	IOCONTEXT_VACUUM,
} IOContext;

#define IOCONTEXT_NUM_TYPES (IOCONTEXT_VACUUM + 1)

typedef enum IOOp
{
	IOOP_EVICT,
	IOOP_EXTEND,
	IOOP_FSYNC,
	IOOP_HIT,
	IOOP_READ,
	IOOP_REUSE,
	IOOP_WRITE,
	IOOP_WRITEBACK,
} IOOp;

#define IOOP_NUM_TYPES (IOOP_WRITEBACK + 1)

typedef struct PgStat_BktypeIO
{
	PgStat_Counter counts[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
	PgStat_Counter times[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
} PgStat_BktypeIO;

typedef struct PgStat_IO
{
	TimestampTz stat_reset_timestamp;
	PgStat_BktypeIO stats[BACKEND_NUM_TYPES];
} PgStat_IO;


typedef struct PgStat_StatDBEntry
{
	PgStat_Counter xact_commit;
	PgStat_Counter xact_rollback;
	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;
	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;
	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	TimestampTz last_autovac_time;
	PgStat_Counter conflict_tablespace;
	PgStat_Counter conflict_lock;
	PgStat_Counter conflict_snapshot;
	PgStat_Counter conflict_logicalslot;
	PgStat_Counter conflict_bufferpin;
	PgStat_Counter conflict_startup_deadlock;
	PgStat_Counter temp_files;
	PgStat_Counter temp_bytes;
	PgStat_Counter deadlocks;
	PgStat_Counter checksum_failures;
	TimestampTz last_checksum_failure;
	PgStat_Counter blk_read_time;	/* times in microseconds */
	PgStat_Counter blk_write_time;
	PgStat_Counter sessions;
	PgStat_Counter session_time;
	PgStat_Counter active_time;
	PgStat_Counter idle_in_transaction_time;
	PgStat_Counter sessions_abandoned;
	PgStat_Counter sessions_fatal;
	PgStat_Counter sessions_killed;

	TimestampTz stat_reset_timestamp;
} PgStat_StatDBEntry;

typedef struct PgStat_StatFuncEntry
{
	PgStat_Counter numcalls;

	PgStat_Counter total_time;	/* times in microseconds */
	PgStat_Counter self_time;
} PgStat_StatFuncEntry;

typedef struct PgStat_StatReplSlotEntry
{
	PgStat_Counter spill_txns;
	PgStat_Counter spill_count;
	PgStat_Counter spill_bytes;
	PgStat_Counter stream_txns;
	PgStat_Counter stream_count;
	PgStat_Counter stream_bytes;
	PgStat_Counter total_txns;
	PgStat_Counter total_bytes;
	TimestampTz stat_reset_timestamp;
} PgStat_StatReplSlotEntry;

typedef struct PgStat_SLRUStats
{
	PgStat_Counter blocks_zeroed;
	PgStat_Counter blocks_hit;
	PgStat_Counter blocks_read;
	PgStat_Counter blocks_written;
	PgStat_Counter blocks_exists;
	PgStat_Counter flush;
	PgStat_Counter truncate;
	TimestampTz stat_reset_timestamp;
} PgStat_SLRUStats;

typedef struct PgStat_StatSubEntry
{
	PgStat_Counter apply_error_count;
	PgStat_Counter sync_error_count;
	TimestampTz stat_reset_timestamp;
} PgStat_StatSubEntry;

typedef struct PgStat_StatTabEntry
{
	PgStat_Counter numscans;
	TimestampTz lastscan;

	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;

	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	PgStat_Counter tuples_hot_updated;
	PgStat_Counter tuples_newpage_updated;

	PgStat_Counter live_tuples;
	PgStat_Counter dead_tuples;
	PgStat_Counter mod_since_analyze;
	PgStat_Counter ins_since_vacuum;

	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;

	TimestampTz last_vacuum_time;	/* user initiated vacuum */
	PgStat_Counter vacuum_count;
	TimestampTz last_autovacuum_time;	/* autovacuum initiated */
	PgStat_Counter autovacuum_count;
	TimestampTz last_analyze_time;	/* user initiated */
	PgStat_Counter analyze_count;
	TimestampTz last_autoanalyze_time;	/* autovacuum initiated */
	PgStat_Counter autoanalyze_count;
} PgStat_StatTabEntry;

typedef struct PgStat_WalStats
{
	PgStat_Counter wal_records;
	PgStat_Counter wal_fpi;
	uint64		wal_bytes;
	PgStat_Counter wal_buffers_full;
	PgStat_Counter wal_write;
	PgStat_Counter wal_sync;
	PgStat_Counter wal_write_time;
	PgStat_Counter wal_sync_time;
	TimestampTz stat_reset_timestamp;
} PgStat_WalStats;

/*
 * This struct stores wal-related durations as instr_time, which makes it
 * cheaper and easier to accumulate them, by not requiring type
 * conversions. During stats flush instr_time will be converted into
 * microseconds.
 */
typedef struct PgStat_PendingWalStats
{
<<<<<<< HEAD
	PgStat_Counter blocks_zeroed;
	PgStat_Counter blocks_hit;
	PgStat_Counter blocks_read;
	PgStat_Counter blocks_written;
	PgStat_Counter blocks_exists;
	PgStat_Counter flush;
	PgStat_Counter truncate;
	TimestampTz stat_reset_timestamp;
} PgStat_SLRUStats;



/*
 * Replication slot statistics kept in the stats collector
 */
typedef struct PgStat_StatReplSlotEntry
{
	NameData	slotname;
	PgStat_Counter spill_txns;
	PgStat_Counter spill_count;
	PgStat_Counter spill_bytes;
	PgStat_Counter stream_txns;
	PgStat_Counter stream_count;
	PgStat_Counter stream_bytes;
	PgStat_Counter total_txns;
	PgStat_Counter total_bytes;
	TimestampTz stat_reset_timestamp;
} PgStat_StatReplSlotEntry;
=======
	PgStat_Counter wal_buffers_full;
	PgStat_Counter wal_write;
	PgStat_Counter wal_sync;
	instr_time	wal_write_time;
	instr_time	wal_sync_time;
} PgStat_PendingWalStats;
>>>>>>> REL_16_9




/*
 * Functions in pgstat.c
 */

/* functions called from postmaster */
extern Size StatsShmemSize(void);
extern void StatsShmemInit(void);

/* Functions called during server startup / shutdown */
extern void pgstat_restore_stats(void);
extern void pgstat_discard_stats(void);
extern void pgstat_before_server_shutdown(int code, Datum arg);

<<<<<<< HEAD
extern bool pgstat_collect_queuelevel;


/*
 * BgWriter statistics counters are updated directly by bgwriter and bufmgr
 */
extern PgStat_MsgBgWriter BgWriterStats;
=======
/* Functions for backend initialization */
extern void pgstat_initialize(void);
>>>>>>> REL_16_9

/* Functions called from backends */
extern long pgstat_report_stat(bool force);
extern void pgstat_force_next_flush(void);

<<<<<<< HEAD
/*
 * Updated by pgstat_count_buffer_*_time macros
 */
extern PgStat_Counter pgStatBlockReadTime;
extern PgStat_Counter pgStatBlockWriteTime;

/*
 * Updated by pgstat_count_conn_*_time macros, called by
 * pgstat_report_activity().
 */
extern PgStat_Counter pgStatActiveTime;
extern PgStat_Counter pgStatTransactionIdleTime;


/*
 * Updated by the traffic cop and in errfinish()
 */
extern SessionEndType pgStatSessionEndCause;

/* ----------
 * Functions called from postmaster
 * ----------
 */
extern void pgstat_init(void);
extern int	pgstat_start(void);
extern void pgstat_reset_all(void);
extern void allow_immediate_pgstat_restart(void);

#ifdef EXEC_BACKEND
extern void PgstatCollectorMain(int argc, char *argv[]) pg_attribute_noreturn();
#endif


/* ----------
 * Functions called from backends
 * ----------
 */
extern void pgstat_ping(void);

extern void pgstat_report_stat(bool force);
extern void pgstat_vacuum_stat(void);
extern void pgstat_report_queuestat(void); /* GPDB */

extern void pgstat_drop_database(Oid databaseid);

extern void pgstat_clear_snapshot(void);
=======
>>>>>>> REL_16_9
extern void pgstat_reset_counters(void);
extern void pgstat_reset(PgStat_Kind kind, Oid dboid, Oid objoid);
extern void pgstat_reset_of_kind(PgStat_Kind kind);

<<<<<<< HEAD
extern void pgstat_report_connect(Oid dboid);
extern void pgstat_report_autovac(Oid dboid);
extern void pgstat_report_vacuum(Oid tableoid, bool shared,
								 PgStat_Counter livetuples, PgStat_Counter deadtuples);
extern void pgstat_report_analyze(Relation rel,
								  PgStat_Counter livetuples, PgStat_Counter deadtuples,
								  bool resetcounter);

=======
/* stats accessors */
extern void pgstat_clear_snapshot(void);
extern TimestampTz pgstat_get_stat_snapshot_timestamp(bool *have_snapshot);

/* helpers */
extern PgStat_Kind pgstat_get_kind_from_str(char *kind_str);
extern bool pgstat_have_entry(PgStat_Kind kind, Oid dboid, Oid objoid);


/*
 * Functions in pgstat_archiver.c
 */

extern void pgstat_report_archiver(const char *xlog, bool failed);
extern PgStat_ArchiverStats *pgstat_fetch_stat_archiver(void);


/*
 * Functions in pgstat_bgwriter.c
 */

extern void pgstat_report_bgwriter(void);
extern PgStat_BgWriterStats *pgstat_fetch_stat_bgwriter(void);


/*
 * Functions in pgstat_checkpointer.c
 */

extern void pgstat_report_checkpointer(void);
extern PgStat_CheckpointerStats *pgstat_fetch_stat_checkpointer(void);


/*
 * Functions in pgstat_io.c
 */

extern bool pgstat_bktype_io_stats_valid(PgStat_BktypeIO *backend_io,
										 BackendType bktype);
extern void pgstat_count_io_op(IOObject io_object, IOContext io_context, IOOp io_op);
extern void pgstat_count_io_op_n(IOObject io_object, IOContext io_context, IOOp io_op, uint32 cnt);
extern instr_time pgstat_prepare_io_time(void);
extern void pgstat_count_io_op_time(IOObject io_object, IOContext io_context,
									IOOp io_op, instr_time start_time, uint32 cnt);

extern PgStat_IO *pgstat_fetch_stat_io(void);
extern const char *pgstat_get_io_context_name(IOContext io_context);
extern const char *pgstat_get_io_object_name(IOObject io_object);

extern bool pgstat_tracks_io_bktype(BackendType bktype);
extern bool pgstat_tracks_io_object(BackendType bktype,
									IOObject io_object, IOContext io_context);
extern bool pgstat_tracks_io_op(BackendType bktype, IOObject io_object,
								IOContext io_context, IOOp io_op);


/*
 * Functions in pgstat_database.c
 */

extern void pgstat_drop_database(Oid databaseid);
extern void pgstat_report_autovac(Oid dboid);
>>>>>>> REL_16_9
extern void pgstat_report_recovery_conflict(int reason);
extern void pgstat_report_deadlock(void);
extern void pgstat_report_checksum_failures_in_db(Oid dboid, int failurecount);
extern void pgstat_report_checksum_failure(void);
extern void pgstat_report_connect(Oid dboid);

<<<<<<< HEAD
extern void pgstat_initialize(void);


extern void pgstat_report_sessionid(int new_sessionid);




extern PgStat_TableStatus *find_tabstat_entry(Oid rel_id);
extern PgStat_BackendFunctionEntry *find_funcstat_entry(Oid func_id);

extern void pgstat_report_resgroup(Oid groupid);

extern void pgstat_initstats(Relation rel);


extern char *pgstat_clip_activity(const char *raw_activity);

extern void pgstat_init_localportalhash(void);
extern PgStat_StatPortalEntry *pgstat_getportalentry(uint32 portalid,
													 Oid queueid);


/* nontransactional event counts are simple enough to inline */

#define pgstat_count_heap_scan(rel)									\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_numscans++;				\
	} while (0)
#define pgstat_count_heap_getnext(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_tuples_returned++;		\
	} while (0)
#define pgstat_count_heap_fetch(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_tuples_fetched++;		\
	} while (0)

/*
#define pgstat_count_heap_insert(s)										\
	do {																\
		if (pgstat_track_counts)			\
			(s)->pgstat_info->t_counts.t_tuples_inserted++; \
	} while (0)
#define pgstat_count_heap_update(s,h)										\
	do {																\
		if (pgstat_track_counts)			\
			(s)->pgstat_info->t_counts.t_tuples_updated++; \
	} while (0)
#define pgstat_count_heap_delete(s)										\
	do {																\
		if (pgstat_track_counts)			\
			(s)->pgstat_info->t_counts.t_tuples_deleted++; \
	} while (0)
	*/

#define pgstat_count_index_scan(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_numscans++;				\
	} while (0)
#define pgstat_count_index_tuples(rel, n)							\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_tuples_returned += (n);	\
	} while (0)
#define pgstat_count_buffer_read(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_blocks_fetched++;		\
	} while (0)
#define pgstat_count_buffer_hit(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_blocks_hit++;			\
	} while (0)
=======
>>>>>>> REL_16_9
#define pgstat_count_buffer_read_time(n)							\
	(pgStatBlockReadTime += (n))
#define pgstat_count_buffer_write_time(n)							\
	(pgStatBlockWriteTime += (n))
#define pgstat_count_conn_active_time(n)							\
	(pgStatActiveTime += (n))
#define pgstat_count_conn_txn_idle_time(n)							\
	(pgStatTransactionIdleTime += (n))

<<<<<<< HEAD
/* Resource queue statistics: */
#define pgstat_count_queue_exec(p, q) 									\
	do {																\
		if (pgstat_collect_queuelevel) 									\
		{																\
			PgStat_StatPortalEntry  *pentry;							\
																		\
			pentry = pgstat_getportalentry(p, q);						\
			((pentry)->queueentry).n_queries_exec++;					\
		}																\
	} while (0)
#define pgstat_record_start_queue_exec(p, q) 							\
	do {																\
		if (pgstat_collect_queuelevel) 									\
		{																\
			PgStat_StatPortalEntry  *pentry;							\
																		\
			pentry = pgstat_getportalentry(p, q);						\
			(pentry)->t_exec_start = time(NULL);						\
		}																\
	} while (0)
#define pgstat_record_end_queue_exec(p, q) 								\
	do {																\
		if (pgstat_collect_queuelevel) 									\
		{																\
			PgStat_StatPortalEntry  *pentry;							\
																		\
			pentry = pgstat_getportalentry(p, q);						\
			(pentry)->t_exec_end = time(NULL);							\
			((pentry)->queueentry).elapsed_exec += 						\
				(PgStat_Counter)((pentry)->t_exec_end - (pentry)->t_exec_start);			\
		}																\
	} while (0)
#define pgstat_count_queue_wait(p, q) 									\
	do {																\
		if (pgstat_collect_queuelevel) 									\
		{																\
			PgStat_StatPortalEntry  *pentry;							\
																		\
			pentry = pgstat_getportalentry(p, q);						\
			((pentry)->queueentry).n_queries_wait++;					\
		}																\
	} while (0)
#define pgstat_record_start_queue_wait(p, q) 							\
	do {																\
		if (pgstat_collect_queuelevel) 									\
		{																\
			PgStat_StatPortalEntry  *pentry;							\
																		\
			pentry = pgstat_getportalentry(p, q);						\
			(pentry)->t_wait_start = time(NULL);						\
		}																\
	} while (0)
#define pgstat_record_end_queue_wait(p, q) 								\
	do {																\
		if (pgstat_collect_queuelevel) 									\
		{																\
			PgStat_StatPortalEntry  *pentry;							\
																		\
			pentry = pgstat_getportalentry(p, q);						\
			(pentry)->t_wait_end = time(NULL);							\
			((pentry)->queueentry).elapsed_wait += 						\
				(PgStat_Counter)((pentry)->t_wait_end - (pentry)->t_wait_start);			\
		}																\
	} while (0)
	
extern void pgstat_count_heap_insert(Relation rel, PgStat_Counter n);
extern void pgstat_count_heap_update(Relation rel, bool hot);
extern void pgstat_count_heap_delete(Relation rel);
extern void pgstat_count_truncate(Relation rel);
extern void pgstat_update_heap_dead_tuples(Relation rel, int delta);
=======
extern PgStat_StatDBEntry *pgstat_fetch_stat_dbentry(Oid dboid);


/*
 * Functions in pgstat_function.c
 */

extern void pgstat_create_function(Oid proid);
extern void pgstat_drop_function(Oid proid);
>>>>>>> REL_16_9

struct FunctionCallInfoBaseData;
extern void pgstat_init_function_usage(struct FunctionCallInfoBaseData *fcinfo,
									   PgStat_FunctionCallUsage *fcu);
extern void pgstat_end_function_usage(PgStat_FunctionCallUsage *fcu,
									  bool finalize);

extern PgStat_StatFuncEntry *pgstat_fetch_stat_funcentry(Oid func_id);
extern PgStat_FunctionCounts *find_funcstat_entry(Oid func_id);


/*
 * Functions in pgstat_relation.c
 */

extern void pgstat_create_relation(Relation rel);
extern void pgstat_drop_relation(Relation rel);
extern void pgstat_copy_relation_stats(Relation dst, Relation src);

extern void pgstat_init_relation(Relation rel);
extern void pgstat_assoc_relation(Relation rel);
extern void pgstat_unlink_relation(Relation rel);

extern void pgstat_report_vacuum(Oid tableoid, bool shared,
								 PgStat_Counter livetuples, PgStat_Counter deadtuples);
extern void pgstat_report_analyze(Relation rel,
								  PgStat_Counter livetuples, PgStat_Counter deadtuples,
								  bool resetcounter);

/*
 * If stats are enabled, but pending data hasn't been prepared yet, call
 * pgstat_assoc_relation() to do so. See its comment for why this is done
 * separately from pgstat_init_relation().
 */
#define pgstat_should_count_relation(rel)                           \
	(likely((rel)->pgstat_info != NULL) ? true :                    \
	 ((rel)->pgstat_enabled ? pgstat_assoc_relation(rel), true : false))

/* nontransactional event counts are simple enough to inline */

#define pgstat_count_heap_scan(rel)									\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.numscans++;					\
	} while (0)
#define pgstat_count_heap_getnext(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.tuples_returned++;			\
	} while (0)
#define pgstat_count_heap_fetch(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.tuples_fetched++;			\
	} while (0)
#define pgstat_count_index_scan(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.numscans++;					\
	} while (0)
#define pgstat_count_index_tuples(rel, n)							\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.tuples_returned += (n);		\
	} while (0)
#define pgstat_count_buffer_read(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.blocks_fetched++;			\
	} while (0)
#define pgstat_count_buffer_hit(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.blocks_hit++;				\
	} while (0)

extern void pgstat_count_heap_insert(Relation rel, PgStat_Counter n);
extern void pgstat_count_heap_update(Relation rel, bool hot, bool newpage);
extern void pgstat_count_heap_delete(Relation rel);
extern void pgstat_count_truncate(Relation rel);
extern void pgstat_update_heap_dead_tuples(Relation rel, int delta);

extern void pgstat_twophase_postcommit(TransactionId xid, uint16 info,
									   void *recdata, uint32 len);
extern void pgstat_twophase_postabort(TransactionId xid, uint16 info,
									  void *recdata, uint32 len);

<<<<<<< HEAD
extern void pgstat_send_archiver(const char *xlog, bool failed);
extern void pgstat_send_bgwriter(void);
extern void pgstat_send_wal(bool force);

struct CdbDispatchResults;
struct pg_result;
extern void pgstat_send_qd_tabstats(void);								/* GPDB */
extern void pgstat_combine_one_qe_result(List **oidList,           /* GPDB */
										 struct pg_result *pgresult,
										 int nest_level,
										 int32 segindex);
extern void pgstat_combine_from_qe(struct CdbDispatchResults *results,	/* GPDB */
								   int writerSliceIndex);

/* ----------
 * Support functions for the SQL-callable functions to
 * generate the pgstat* views.
 * ----------
 */
extern PgStat_StatDBEntry *pgstat_fetch_stat_dbentry(Oid dbid);
extern PgStat_StatTabEntry *pgstat_fetch_stat_tabentry(Oid relid);

extern PgStat_StatQueueEntry *pgstat_fetch_stat_queueentry(Oid queueid);  /* GPDB */
extern PgBackendStatus *pgstat_fetch_stat_beentry(int beid);
extern LocalPgBackendStatus *pgstat_fetch_stat_local_beentry(int beid);

extern PgStat_StatFuncEntry *pgstat_fetch_stat_funcentry(Oid funcid);
extern PgStat_ArchiverStats *pgstat_fetch_stat_archiver(void);
extern PgStat_GlobalStats *pgstat_fetch_global(void);
extern PgStat_WalStats *pgstat_fetch_stat_wal(void);
extern PgStat_SLRUStats *pgstat_fetch_slru(void);
=======
extern PgStat_StatTabEntry *pgstat_fetch_stat_tabentry(Oid relid);
extern PgStat_StatTabEntry *pgstat_fetch_stat_tabentry_ext(bool shared,
														   Oid reloid);
extern PgStat_TableStatus *find_tabstat_entry(Oid rel_id);


/*
 * Functions in pgstat_replslot.c
 */

extern void pgstat_reset_replslot(const char *name);
struct ReplicationSlot;
extern void pgstat_report_replslot(struct ReplicationSlot *slot, const PgStat_StatReplSlotEntry *repSlotStat);
extern void pgstat_create_replslot(struct ReplicationSlot *slot);
extern void pgstat_acquire_replslot(struct ReplicationSlot *slot);
extern void pgstat_drop_replslot(struct ReplicationSlot *slot);
>>>>>>> REL_16_9
extern PgStat_StatReplSlotEntry *pgstat_fetch_replslot(NameData slotname);


/*
 * Functions in pgstat_slru.c
 */

extern void pgstat_reset_slru(const char *);
extern void pgstat_count_slru_page_zeroed(int slru_idx);
extern void pgstat_count_slru_page_hit(int slru_idx);
extern void pgstat_count_slru_page_read(int slru_idx);
extern void pgstat_count_slru_page_written(int slru_idx);
extern void pgstat_count_slru_page_exists(int slru_idx);
extern void pgstat_count_slru_flush(int slru_idx);
extern void pgstat_count_slru_truncate(int slru_idx);
extern const char *pgstat_get_slru_name(int slru_idx);
extern int	pgstat_get_slru_index(const char *name);
extern PgStat_SLRUStats *pgstat_fetch_slru(void);


/*
 * Functions in pgstat_subscription.c
 */

extern void pgstat_report_subscription_error(Oid subid, bool is_apply_error);
extern void pgstat_create_subscription(Oid subid);
extern void pgstat_drop_subscription(Oid subid);
extern PgStat_StatSubEntry *pgstat_fetch_stat_subscription(Oid subid);


/*
 * Functions in pgstat_xact.c
 */

extern void AtEOXact_PgStat(bool isCommit, bool parallel);
extern void AtEOSubXact_PgStat(bool isCommit, int nestDepth);
extern void AtPrepare_PgStat(void);
extern void PostPrepare_PgStat(void);
struct xl_xact_stats_item;
extern int	pgstat_get_transactional_drops(bool isCommit, struct xl_xact_stats_item **items);
extern void pgstat_execute_transactional_drops(int ndrops, struct xl_xact_stats_item *items, bool is_redo);


/*
 * Functions in pgstat_wal.c
 */

extern void pgstat_report_wal(bool force);
extern PgStat_WalStats *pgstat_fetch_stat_wal(void);


/*
 * Variables in pgstat.c
 */

/* GUC parameters */
extern PGDLLIMPORT bool pgstat_track_counts;
extern PGDLLIMPORT int pgstat_track_functions;
extern PGDLLIMPORT int pgstat_fetch_consistency;


/*
 * Variables in pgstat_bgwriter.c
 */

/* updated directly by bgwriter and bufmgr */
extern PGDLLIMPORT PgStat_BgWriterStats PendingBgWriterStats;


/*
 * Variables in pgstat_checkpointer.c
 */

/*
 * Checkpointer statistics counters are updated directly by checkpointer and
 * bufmgr.
 */
extern PGDLLIMPORT PgStat_CheckpointerStats PendingCheckpointerStats;


/*
 * Variables in pgstat_database.c
 */

/* Updated by pgstat_count_buffer_*_time macros */
extern PGDLLIMPORT PgStat_Counter pgStatBlockReadTime;
extern PGDLLIMPORT PgStat_Counter pgStatBlockWriteTime;

/*
 * Updated by pgstat_count_conn_*_time macros, called by
 * pgstat_report_activity().
 */
extern PGDLLIMPORT PgStat_Counter pgStatActiveTime;
extern PGDLLIMPORT PgStat_Counter pgStatTransactionIdleTime;

/* updated by the traffic cop and in errfinish() */
extern PGDLLIMPORT SessionEndType pgStatSessionEndCause;


/*
 * Variables in pgstat_wal.c
 */

/* updated directly by backends and background processes */
extern PGDLLIMPORT PgStat_PendingWalStats PendingWalStats;


#endif							/* PGSTAT_H */

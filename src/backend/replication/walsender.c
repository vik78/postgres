/*-------------------------------------------------------------------------
 *
 * walsender.c
 *
 * The WAL sender process (walsender) is new as of Postgres 9.0. It takes
 * care of sending XLOG from the primary server to a single recipient.
 * (Note that there can be more than one walsender process concurrently.)
 * It is started by the postmaster when the walreceiver of a standby server
 * connects to the primary server and requests XLOG streaming replication.
 * It attempts to keep reading XLOG records from the disk and sending them
 * to the standby server, as long as the connection is alive (i.e., like
 * any backend, there is a one-to-one relationship between a connection
 * and a walsender process).
 *
 * Normal termination is by SIGTERM, which instructs the walsender to
 * close the connection and exit(0) at next convenient moment. Emergency
 * termination is by SIGQUIT; like any backend, the walsender will simply
 * abort and exit on SIGQUIT. A close of the connection and a FATAL error
 * are treated as not a crash but approximately normal termination;
 * the walsender will exit quickly without sending any more XLOG records.
 *
 * If the server is shut down, postmaster sends us SIGUSR2 after all
 * regular backends have exited and the shutdown checkpoint has been written.
 * This instruct walsender to send any outstanding WAL, including the
 * shutdown checkpoint record, and then exit.
 *
 *
 * Portions Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/walsender.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "funcapi.h"
#include "access/xlog_internal.h"
#include "catalog/pg_type.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "replication/basebackup.h"
#include "replication/replnodes.h"
#include "replication/walprotocol.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"


/* Array of WalSnds in shared memory */
WalSndCtlData *WalSndCtl = NULL;

/* My slot in the shared memory array */
static WalSnd *MyWalSnd = NULL;

/* Global state */
bool		am_walsender = false;		/* Am I a walsender process ? */

/* User-settable parameters for walsender */
int			max_wal_senders = 0;	/* the maximum number of concurrent walsenders */
int			WalSndDelay = 200;	/* max sleep time between some actions */

/*
 * These variables are used similarly to openLogFile/Id/Seg/Off,
 * but for walsender to read the XLOG.
 */
static int	sendFile = -1;
static uint32 sendId = 0;
static uint32 sendSeg = 0;
static uint32 sendOff = 0;

/*
 * How far have we sent WAL already? This is also advertised in
 * MyWalSnd->sentPtr.  (Actually, this is the next WAL location to send.)
 */
static XLogRecPtr sentPtr = {0, 0};

/* Flags set by signal handlers for later service in main loop */
static volatile sig_atomic_t got_SIGHUP = false;
volatile sig_atomic_t walsender_shutdown_requested = false;
volatile sig_atomic_t walsender_ready_to_stop = false;

/* Signal handlers */
static void WalSndSigHupHandler(SIGNAL_ARGS);
static void WalSndShutdownHandler(SIGNAL_ARGS);
static void WalSndQuickDieHandler(SIGNAL_ARGS);
static void WalSndXLogSendHandler(SIGNAL_ARGS);
static void WalSndLastCycleHandler(SIGNAL_ARGS);

/* Prototypes for private functions */
static bool HandleReplicationCommand(const char *cmd_string);
static int	WalSndLoop(void);
static void InitWalSnd(void);
static void WalSndHandshake(void);
static void WalSndKill(int code, Datum arg);
static void XLogRead(char *buf, XLogRecPtr recptr, Size nbytes);
static bool XLogSend(char *msgbuf, bool *caughtup);
static void CheckClosedConnection(void);
static void IdentifySystem(void);
static void StartReplication(StartReplicationCmd * cmd);


/* Main entry point for walsender process */
int
WalSenderMain(void)
{
	MemoryContext walsnd_context;

	if (RecoveryInProgress())
		ereport(FATAL,
				(errcode(ERRCODE_CANNOT_CONNECT_NOW),
				 errmsg("recovery is still in progress, can't accept WAL streaming connections")));

	/* Create a per-walsender data structure in shared memory */
	InitWalSnd();

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 *
	 * XXX: we don't actually attempt error recovery in walsender, we just
	 * close the connection and exit.
	 */
	walsnd_context = AllocSetContextCreate(TopMemoryContext,
										   "Wal Sender",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(walsnd_context);

	/* Set up resource owner */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "walsender top-level resource owner");

	/* Unblock signals (they were blocked when the postmaster forked us) */
	PG_SETMASK(&UnBlockSig);

	/* Tell the standby that walsender is ready for receiving commands */
	ReadyForQuery(DestRemote);

	/* Handle handshake messages before streaming */
	WalSndHandshake();

	/* Initialize shared memory status */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}

	/* Main loop of walsender */
	return WalSndLoop();
}

/*
 * Execute commands from walreceiver, until we enter streaming mode.
 */
static void
WalSndHandshake(void)
{
	StringInfoData input_message;
	bool		replication_started = false;

	initStringInfo(&input_message);

	while (!replication_started)
	{
		int			firstchar;

		WalSndSetState(WALSNDSTATE_STARTUP);
		set_ps_display("idle", false);

		/* Wait for a command to arrive */
		firstchar = pq_getbyte();

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			exit(1);

		/*
		 * Check for any other interesting events that happened while we
		 * slept.
		 */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (firstchar != EOF)
		{
			/*
			 * Read the message contents. This is expected to be done without
			 * blocking because we've been able to get message type code.
			 */
			if (pq_getmessage(&input_message, 0))
				firstchar = EOF;	/* suitable message already logged */
		}

		/* Handle the very limited subset of commands expected in this phase */
		switch (firstchar)
		{
			case 'Q':			/* Query message */
				{
					const char *query_string;

					query_string = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					if (HandleReplicationCommand(query_string))
						replication_started = true;
				}
				break;

			case 'X':
				/* standby is closing the connection */
				proc_exit(0);

			case EOF:
				/* standby disconnected unexpectedly */
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unexpected EOF on standby connection")));
				proc_exit(0);

			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid standby handshake message type %d", firstchar)));
		}
	}
}

/*
 * IDENTIFY_SYSTEM
 */
static void
IdentifySystem(void)
{
	StringInfoData buf;
	char		sysid[32];
	char		tli[11];

	/*
	 * Reply with a result set with one row, two columns. First col is system
	 * ID, and second is timeline ID
	 */

	snprintf(sysid, sizeof(sysid), UINT64_FORMAT,
			 GetSystemIdentifier());
	snprintf(tli, sizeof(tli), "%u", ThisTimeLineID);

	/* Send a RowDescription message */
	pq_beginmessage(&buf, 'T');
	pq_sendint(&buf, 2, 2);		/* 2 fields */

	/* first field */
	pq_sendstring(&buf, "systemid");	/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* second field */
	pq_sendstring(&buf, "timeline");	/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, INT4OID, 4);		/* type oid */
	pq_sendint(&buf, 4, 2);		/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */
	pq_endmessage(&buf);

	/* Send a DataRow message */
	pq_beginmessage(&buf, 'D');
	pq_sendint(&buf, 2, 2);		/* # of columns */
	pq_sendint(&buf, strlen(sysid), 4); /* col1 len */
	pq_sendbytes(&buf, (char *) &sysid, strlen(sysid));
	pq_sendint(&buf, strlen(tli), 4);	/* col2 len */
	pq_sendbytes(&buf, (char *) tli, strlen(tli));
	pq_endmessage(&buf);

	/* Send CommandComplete and ReadyForQuery messages */
	EndCommand("SELECT", DestRemote);
	ReadyForQuery(DestRemote);
	/* ReadyForQuery did pq_flush for us */
}

/*
 * START_REPLICATION
 */
static void
StartReplication(StartReplicationCmd * cmd)
{
	StringInfoData buf;

	/*
	 * Let postmaster know that we're streaming. Once we've declared us as
	 * a WAL sender process, postmaster will let us outlive the bgwriter and
	 * kill us last in the shutdown sequence, so we get a chance to stream
	 * all remaining WAL at shutdown, including the shutdown checkpoint.
	 * Note that there's no going back, and we mustn't write any WAL records
	 * after this.
	 */
	MarkPostmasterChildWalSender();

	/*
	 * Check that we're logging enough information in the WAL for
	 * log-shipping.
	 *
	 * NOTE: This only checks the current value of wal_level. Even if the
	 * current setting is not 'minimal', there can be old WAL in the pg_xlog
	 * directory that was created with 'minimal'. So this is not bulletproof,
	 * the purpose is just to give a user-friendly error message that hints
	 * how to configure the system correctly.
	 */
	if (wal_level == WAL_LEVEL_MINIMAL)
		ereport(FATAL,
				(errcode(ERRCODE_CANNOT_CONNECT_NOW),
		errmsg("standby connections not allowed because wal_level=minimal")));

	/* Send a CopyBothResponse message, and start streaming */
	pq_beginmessage(&buf, 'W');
	pq_sendbyte(&buf, 0);
	pq_sendint(&buf, 0, 2);
	pq_endmessage(&buf);
	pq_flush();

	/*
	 * Initialize position to the received one, then the xlog records begin to
	 * be shipped from that position
	 */
	sentPtr = cmd->startpoint;
}

/*
 * Execute an incoming replication command.
 */
static bool
HandleReplicationCommand(const char *cmd_string)
{
	bool		replication_started = false;
	int			parse_rc;
	Node	   *cmd_node;
	MemoryContext cmd_context;
	MemoryContext old_context;

	elog(DEBUG1, "received replication command: %s", cmd_string);

	cmd_context = AllocSetContextCreate(CurrentMemoryContext,
										"Replication command context",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);
	old_context = MemoryContextSwitchTo(cmd_context);

	replication_scanner_init(cmd_string);
	parse_rc = replication_yyparse();
	if (parse_rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 (errmsg_internal("replication command parser returned %d",
								  parse_rc))));

	cmd_node = replication_parse_result;

	switch (cmd_node->type)
	{
		case T_IdentifySystemCmd:
			IdentifySystem();
			break;

		case T_StartReplicationCmd:
			StartReplication((StartReplicationCmd *) cmd_node);

			/* break out of the loop */
			replication_started = true;
			break;

		case T_BaseBackupCmd:
			{
				BaseBackupCmd *cmd = (BaseBackupCmd *) cmd_node;

				SendBaseBackup(cmd->label, cmd->progress, cmd->fastcheckpoint);

				/* Send CommandComplete and ReadyForQuery messages */
				EndCommand("SELECT", DestRemote);
				ReadyForQuery(DestRemote);
				/* ReadyForQuery did pq_flush for us */
				break;
			}

		default:
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid standby query string: %s", cmd_string)));
	}

	/* done */
	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(cmd_context);

	return replication_started;
}

/*
 * Check if the remote end has closed the connection.
 */
static void
CheckClosedConnection(void)
{
	unsigned char firstchar;
	int			r;

	r = pq_getbyte_if_available(&firstchar);
	if (r < 0)
	{
		/* unexpected error or EOF */
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("unexpected EOF on standby connection")));
		proc_exit(0);
	}
	if (r == 0)
	{
		/* no data available without blocking */
		return;
	}

	/* Handle the very limited subset of commands expected in this phase */
	switch (firstchar)
	{
			/*
			 * 'X' means that the standby is closing down the socket.
			 */
		case 'X':
			proc_exit(0);

		default:
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid standby closing message type %d",
							firstchar)));
	}
}

/* Main loop of walsender process */
static int
WalSndLoop(void)
{
	char	   *output_message;
	bool		caughtup = false;

	/*
	 * Allocate buffer that will be used for each output message.  We do this
	 * just once to reduce palloc overhead.  The buffer must be made large
	 * enough for maximum-sized messages.
	 */
	output_message = palloc(1 + sizeof(WalDataMessageHeader) + MAX_SEND_SIZE);

	/* Loop forever, unless we get an error */
	for (;;)
	{
		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			exit(1);

		/* Process any requests or signals received recently */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * When SIGUSR2 arrives, we send all outstanding logs up to the
		 * shutdown checkpoint record (i.e., the latest record) and exit.
		 */
		if (walsender_ready_to_stop)
		{
			if (!XLogSend(output_message, &caughtup))
				break;
			if (caughtup)
				walsender_shutdown_requested = true;
		}

		/* Normal exit from the walsender is here */
		if (walsender_shutdown_requested)
		{
			/* Inform the standby that XLOG streaming was done */
			pq_puttextmessage('C', "COPY 0");
			pq_flush();

			proc_exit(0);
		}

		/*
		 * If we had sent all accumulated WAL in last round, nap for the
		 * configured time before retrying.
		 */
		if (caughtup)
		{
			/*
			 * Even if we wrote all the WAL that was available when we started
			 * sending, more might have arrived while we were sending this
			 * batch. We had the latch set while sending, so we have not
			 * received any signals from that time. Let's arm the latch
			 * again, and after that check that we're still up-to-date.
			 */
			ResetLatch(&MyWalSnd->latch);

			if (!XLogSend(output_message, &caughtup))
				break;
			if (caughtup && !got_SIGHUP && !walsender_ready_to_stop && !walsender_shutdown_requested)
			{
				/*
				 * XXX: We don't really need the periodic wakeups anymore,
				 * WaitLatchOrSocket should reliably wake up as soon as
				 * something interesting happens.
				 */

				/* Sleep */
				WaitLatchOrSocket(&MyWalSnd->latch, MyProcPort->sock,
								  WalSndDelay * 1000L);
			}

			/* Check if the connection was closed */
			CheckClosedConnection();
		}
		else
		{
			/* Attempt to send the log once every loop */
			if (!XLogSend(output_message, &caughtup))
				break;
		}

		/* Update our state to indicate if we're behind or not */
		WalSndSetState(caughtup ? WALSNDSTATE_STREAMING : WALSNDSTATE_CATCHUP);
	}

	/*
	 * Get here on send failure.  Clean up and exit.
	 *
	 * Reset whereToSendOutput to prevent ereport from attempting to send any
	 * more messages to the standby.
	 */
	if (whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	proc_exit(0);
	return 1;					/* keep the compiler quiet */
}

/* Initialize a per-walsender data structure for this walsender process */
static void
InitWalSnd(void)
{
	int			i;

	/*
	 * WalSndCtl should be set up already (we inherit this by fork() or
	 * EXEC_BACKEND mechanism from the postmaster).
	 */
	Assert(WalSndCtl != NULL);
	Assert(MyWalSnd == NULL);

	/*
	 * Find a free walsender slot and reserve it. If this fails, we must be
	 * out of WalSnd structures.
	 */
	for (i = 0; i < max_wal_senders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];

		SpinLockAcquire(&walsnd->mutex);

		if (walsnd->pid != 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		else
		{
			/*
			 * Found a free slot. Reserve it for us.
			 */
			walsnd->pid = MyProcPid;
			MemSet(&walsnd->sentPtr, 0, sizeof(XLogRecPtr));
			walsnd->state = WALSNDSTATE_STARTUP;
			SpinLockRelease(&walsnd->mutex);
			/* don't need the lock anymore */
			OwnLatch((Latch *) &walsnd->latch);
			MyWalSnd = (WalSnd *) walsnd;

			break;
		}
	}
	if (MyWalSnd == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("number of requested standby connections "
						"exceeds max_wal_senders (currently %d)",
						max_wal_senders)));

	/* Arrange to clean up at walsender exit */
	on_shmem_exit(WalSndKill, 0);
}

/* Destroy the per-walsender data structure for this walsender process */
static void
WalSndKill(int code, Datum arg)
{
	Assert(MyWalSnd != NULL);

	/*
	 * Mark WalSnd struct no longer in use. Assume that no lock is required
	 * for this.
	 */
	MyWalSnd->pid = 0;
	DisownLatch(&MyWalSnd->latch);

	/* WalSnd struct isn't mine anymore */
	MyWalSnd = NULL;
}

/*
 * Read 'nbytes' bytes from WAL into 'buf', starting at location 'recptr'
 *
 * XXX probably this should be improved to suck data directly from the
 * WAL buffers when possible.
 */
static void
XLogRead(char *buf, XLogRecPtr recptr, Size nbytes)
{
	XLogRecPtr	startRecPtr = recptr;
	char		path[MAXPGPATH];
	uint32		lastRemovedLog;
	uint32		lastRemovedSeg;
	uint32		log;
	uint32		seg;

	while (nbytes > 0)
	{
		uint32		startoff;
		int			segbytes;
		int			readbytes;

		startoff = recptr.xrecoff % XLogSegSize;

		if (sendFile < 0 || !XLByteInSeg(recptr, sendId, sendSeg))
		{
			/* Switch to another logfile segment */
			if (sendFile >= 0)
				close(sendFile);

			XLByteToSeg(recptr, sendId, sendSeg);
			XLogFilePath(path, ThisTimeLineID, sendId, sendSeg);

			sendFile = BasicOpenFile(path, O_RDONLY | PG_BINARY, 0);
			if (sendFile < 0)
			{
				/*
				 * If the file is not found, assume it's because the standby
				 * asked for a too old WAL segment that has already been
				 * removed or recycled.
				 */
				if (errno == ENOENT)
				{
					char		filename[MAXFNAMELEN];

					XLogFileName(filename, ThisTimeLineID, sendId, sendSeg);
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("requested WAL segment %s has already been removed",
									filename)));
				}
				else
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not open file \"%s\" (log file %u, segment %u): %m",
									path, sendId, sendSeg)));
			}
			sendOff = 0;
		}

		/* Need to seek in the file? */
		if (sendOff != startoff)
		{
			if (lseek(sendFile, (off_t) startoff, SEEK_SET) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not seek in log file %u, segment %u to offset %u: %m",
								sendId, sendSeg, startoff)));
			sendOff = startoff;
		}

		/* How many bytes are within this segment? */
		if (nbytes > (XLogSegSize - startoff))
			segbytes = XLogSegSize - startoff;
		else
			segbytes = nbytes;

		readbytes = read(sendFile, buf, segbytes);
		if (readbytes <= 0)
			ereport(ERROR,
					(errcode_for_file_access(),
			errmsg("could not read from log file %u, segment %u, offset %u, "
				   "length %lu: %m",
				   sendId, sendSeg, sendOff, (unsigned long) segbytes)));

		/* Update state for read */
		XLByteAdvance(recptr, readbytes);

		sendOff += readbytes;
		nbytes -= readbytes;
		buf += readbytes;
	}

	/*
	 * After reading into the buffer, check that what we read was valid. We do
	 * this after reading, because even though the segment was present when we
	 * opened it, it might get recycled or removed while we read it. The
	 * read() succeeds in that case, but the data we tried to read might
	 * already have been overwritten with new WAL records.
	 */
	XLogGetLastRemoved(&lastRemovedLog, &lastRemovedSeg);
	XLByteToSeg(startRecPtr, log, seg);
	if (log < lastRemovedLog ||
		(log == lastRemovedLog && seg <= lastRemovedSeg))
	{
		char		filename[MAXFNAMELEN];

		XLogFileName(filename, ThisTimeLineID, log, seg);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("requested WAL segment %s has already been removed",
						filename)));
	}
}

/*
 * Read up to MAX_SEND_SIZE bytes of WAL that's been flushed to disk,
 * but not yet sent to the client, and send it.
 *
 * msgbuf is a work area in which the output message is constructed.  It's
 * passed in just so we can avoid re-palloc'ing the buffer on each cycle.
 * It must be of size 1 + sizeof(WalDataMessageHeader) + MAX_SEND_SIZE.
 *
 * If there is no unsent WAL remaining, *caughtup is set to true, otherwise
 * *caughtup is set to false.
 *
 * Returns true if OK, false if trouble.
 */
static bool
XLogSend(char *msgbuf, bool *caughtup)
{
	XLogRecPtr	SendRqstPtr;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	Size		nbytes;
	WalDataMessageHeader msghdr;

	/*
	 * Attempt to send all data that's already been written out and fsync'd to
	 * disk.  We cannot go further than what's been written out given the
	 * current implementation of XLogRead().  And in any case it's unsafe to
	 * send WAL that is not securely down to disk on the master: if the master
	 * subsequently crashes and restarts, slaves must not have applied any WAL
	 * that gets lost on the master.
	 */
	SendRqstPtr = GetFlushRecPtr();

	/* Quick exit if nothing to do */
	if (XLByteLE(SendRqstPtr, sentPtr))
	{
		*caughtup = true;
		return true;
	}

	/*
	 * Figure out how much to send in one message. If there's no more than
	 * MAX_SEND_SIZE bytes to send, send everything. Otherwise send
	 * MAX_SEND_SIZE bytes, but round back to logfile or page boundary.
	 *
	 * The rounding is not only for performance reasons. Walreceiver relies on
	 * the fact that we never split a WAL record across two messages. Since a
	 * long WAL record is split at page boundary into continuation records,
	 * page boundary is always a safe cut-off point. We also assume that
	 * SendRqstPtr never points to the middle of a WAL record.
	 */
	startptr = sentPtr;
	if (startptr.xrecoff >= XLogFileSize)
	{
		/*
		 * crossing a logid boundary, skip the non-existent last log segment
		 * in previous logical log file.
		 */
		startptr.xlogid += 1;
		startptr.xrecoff = 0;
	}

	endptr = startptr;
	XLByteAdvance(endptr, MAX_SEND_SIZE);
	if (endptr.xlogid != startptr.xlogid)
	{
		/* Don't cross a logfile boundary within one message */
		Assert(endptr.xlogid == startptr.xlogid + 1);
		endptr.xlogid = startptr.xlogid;
		endptr.xrecoff = XLogFileSize;
	}

	/* if we went beyond SendRqstPtr, back off */
	if (XLByteLE(SendRqstPtr, endptr))
	{
		endptr = SendRqstPtr;
		*caughtup = true;
	}
	else
	{
		/* round down to page boundary. */
		endptr.xrecoff -= (endptr.xrecoff % XLOG_BLCKSZ);
		*caughtup = false;
	}

	nbytes = endptr.xrecoff - startptr.xrecoff;
	Assert(nbytes <= MAX_SEND_SIZE);

	/*
	 * OK to read and send the slice.
	 */
	msgbuf[0] = 'w';

	/*
	 * Read the log directly into the output buffer to avoid extra memcpy
	 * calls.
	 */
	XLogRead(msgbuf + 1 + sizeof(WalDataMessageHeader), startptr, nbytes);

	/*
	 * We fill the message header last so that the send timestamp is taken as
	 * late as possible.
	 */
	msghdr.dataStart = startptr;
	msghdr.walEnd = SendRqstPtr;
	msghdr.sendTime = GetCurrentTimestamp();

	memcpy(msgbuf + 1, &msghdr, sizeof(WalDataMessageHeader));

	pq_putmessage('d', msgbuf, 1 + sizeof(WalDataMessageHeader) + nbytes);

	/* Flush pending output to the client */
	if (pq_flush())
		return false;

	sentPtr = endptr;

	/* Update shared memory status */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}

	/* Report progress of XLOG streaming in PS display */
	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
				 sentPtr.xlogid, sentPtr.xrecoff);
		set_ps_display(activitymsg, false);
	}

	return true;
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
WalSndSigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
	if (MyWalSnd)
		SetLatch(&MyWalSnd->latch);
}

/* SIGTERM: set flag to shut down */
static void
WalSndShutdownHandler(SIGNAL_ARGS)
{
	walsender_shutdown_requested = true;
	if (MyWalSnd)
		SetLatch(&MyWalSnd->latch);
}

/*
 * WalSndQuickDieHandler() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
static void
WalSndQuickDieHandler(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);

	/*
	 * We DO NOT want to run proc_exit() callbacks -- we're here because
	 * shared memory may be corrupted, so we don't want to try to clean up our
	 * transaction.  Just nail the windows shut and get out of town.  Now that
	 * there's an atexit callback to prevent third-party code from breaking
	 * things by calling exit() directly, we have to reset the callbacks
	 * explicitly to make this work as intended.
	 */
	on_exit_reset();

	/*
	 * Note we do exit(2) not exit(0).	This is to force the postmaster into a
	 * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm in
	 * being doubly sure.)
	 */
	exit(2);
}

/* SIGUSR1: set flag to send WAL records */
static void
WalSndXLogSendHandler(SIGNAL_ARGS)
{
	latch_sigusr1_handler();
}

/* SIGUSR2: set flag to do a last cycle and shut down afterwards */
static void
WalSndLastCycleHandler(SIGNAL_ARGS)
{
	walsender_ready_to_stop = true;
	if (MyWalSnd)
		SetLatch(&MyWalSnd->latch);
}

/* Set up signal handlers */
void
WalSndSignals(void)
{
	/* Set up signal handlers */
	pqsignal(SIGHUP, WalSndSigHupHandler);		/* set flag to read config
												 * file */
	pqsignal(SIGINT, SIG_IGN);	/* not used */
	pqsignal(SIGTERM, WalSndShutdownHandler);	/* request shutdown */
	pqsignal(SIGQUIT, WalSndQuickDieHandler);	/* hard crash time */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, WalSndXLogSendHandler);	/* request WAL sending */
	pqsignal(SIGUSR2, WalSndLastCycleHandler);	/* request a last cycle and
												 * shutdown */

	/* Reset some signals that are accepted by postmaster but not here */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);
}

/* Report shared-memory space needed by WalSndShmemInit */
Size
WalSndShmemSize(void)
{
	Size		size = 0;

	size = offsetof(WalSndCtlData, walsnds);
	size = add_size(size, mul_size(max_wal_senders, sizeof(WalSnd)));

	return size;
}

/* Allocate and initialize walsender-related shared memory */
void
WalSndShmemInit(void)
{
	bool		found;
	int			i;

	WalSndCtl = (WalSndCtlData *)
		ShmemInitStruct("Wal Sender Ctl", WalSndShmemSize(), &found);

	if (!found)
	{
		/* First time through, so initialize */
		MemSet(WalSndCtl, 0, WalSndShmemSize());

		for (i = 0; i < max_wal_senders; i++)
		{
			WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

			SpinLockInit(&walsnd->mutex);
			InitSharedLatch(&walsnd->latch);
		}
	}
}

/* Wake up all walsenders */
void
WalSndWakeup(void)
{
	int		i;

	for (i = 0; i < max_wal_senders; i++)
		SetLatch(&WalSndCtl->walsnds[i].latch);
}

/* Set state for current walsender (only called in walsender) */
void
WalSndSetState(WalSndState state)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalSnd *walsnd = MyWalSnd;

	Assert(am_walsender);

	if (walsnd->state == state)
		return;

	SpinLockAcquire(&walsnd->mutex);
	walsnd->state = state;
	SpinLockRelease(&walsnd->mutex);
}

/*
 * Return a string constant representing the state. This is used
 * in system views, and should *not* be translated.
 */
static const char *
WalSndGetStateString(WalSndState state)
{
	switch (state)
	{
		case WALSNDSTATE_STARTUP:
			return "STARTUP";
		case WALSNDSTATE_BACKUP:
			return "BACKUP";
		case WALSNDSTATE_CATCHUP:
			return "CATCHUP";
		case WALSNDSTATE_STREAMING:
			return "STREAMING";
	}
	return "UNKNOWN";
}


/*
 * Returns activity of walsenders, including pids and xlog locations sent to
 * standby servers.
 */
Datum
pg_stat_get_wal_senders(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_WAL_SENDERS_COLS 	3
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	int					i;

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

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	for (i = 0; i < max_wal_senders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];
		char		sent_location[MAXFNAMELEN];
		XLogRecPtr	sentPtr;
		WalSndState	state;
		Datum		values[PG_STAT_GET_WAL_SENDERS_COLS];
		bool		nulls[PG_STAT_GET_WAL_SENDERS_COLS];

		if (walsnd->pid == 0)
			continue;

		SpinLockAcquire(&walsnd->mutex);
		sentPtr = walsnd->sentPtr;
		state = walsnd->state;
		SpinLockRelease(&walsnd->mutex);

		snprintf(sent_location, sizeof(sent_location), "%X/%X",
					sentPtr.xlogid, sentPtr.xrecoff);

		memset(nulls, 0, sizeof(nulls));
		values[0] = Int32GetDatum(walsnd->pid);
		values[1] = CStringGetTextDatum(WalSndGetStateString(state));
		values[2] = CStringGetTextDatum(sent_location);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * This isn't currently used for anything. Monitoring tools might be
 * interested in the future, and we'll need something like this in the
 * future for synchronous replication.
 */
#ifdef NOT_USED
/*
 * Returns the oldest Send position among walsenders. Or InvalidXLogRecPtr
 * if none.
 */
XLogRecPtr
GetOldestWALSendPointer(void)
{
	XLogRecPtr	oldest = {0, 0};
	int			i;
	bool		found = false;

	for (i = 0; i < max_wal_senders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];
		XLogRecPtr	recptr;

		if (walsnd->pid == 0)
			continue;

		SpinLockAcquire(&walsnd->mutex);
		recptr = walsnd->sentPtr;
		SpinLockRelease(&walsnd->mutex);

		if (recptr.xlogid == 0 && recptr.xrecoff == 0)
			continue;

		if (!found || XLByteLT(recptr, oldest))
			oldest = recptr;
		found = true;
	}
	return oldest;
}

#endif

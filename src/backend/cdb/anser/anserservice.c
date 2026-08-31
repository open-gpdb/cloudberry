/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * anserservice.c
 *	  Coordinator-local Anser background services.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/anser/anserservice.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/anser.h"
#include "cdb/cdbvars.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/wait_event.h"

static volatile sig_atomic_t anser_service_got_sigterm = false;
static volatile sig_atomic_t anser_service_got_sighup = false;

static void AnserServiceLoop(const char *service_name, bool gather_service);
static void AnserServiceSigHup(SIGNAL_ARGS);
static void AnserServiceSigTerm(SIGNAL_ARGS);

bool
AnserStartRule(Datum main_arg)
{
	return gp_anser_enable && Gp_role == GP_ROLE_DISPATCH;
}

void
AnserGatherServiceMain(Datum main_arg)
{
	AnserServiceLoop("anser gather service", true);
}

void
AnserSendServiceMain(Datum main_arg)
{
	AnserServiceLoop("anser send service", false);
}

static void
AnserServiceLoop(const char *service_name, bool gather_service)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext service_ctx;

	pqsignal(SIGHUP, AnserServiceSigHup);
	pqsignal(SIGTERM, AnserServiceSigTerm);
	BackgroundWorkerUnblockSignals();

	init_ps_display(service_name);
	ereport(LOG,
			(errmsg_internal("%s started", service_name)));

	/*
	 * Do all per-cycle work in a dedicated context so error recovery can reset
	 * it, and under a resource owner so a failed cycle's attached/created DSM
	 * segments are reclaimed rather than leaked.
	 */
	service_ctx = AllocSetContextCreate(TopMemoryContext, "Anser service",
										ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(service_ctx);
	if (CurrentResourceOwner == NULL)
		CurrentResourceOwner = ResourceOwnerCreate(NULL, service_name);

	AnserAttachServiceLatch(gather_service);

	/*
	 * If a cycle raises an error, resume here: log it, drop whatever the cycle
	 * held, and carry on rather than terminating the worker.  Modeled on the
	 * shmem-only auxiliary processes (see bgwriter.c); the leftmost sigsetjmp
	 * stays active so we can even survive an error during recovery.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Not using PG_TRY, so reset the error stack by hand. */
		error_context_stack = NULL;

		HOLD_INTERRUPTS();

		EmitErrorReport();

		/* Minimal subset of AbortTransaction() for a shmem-only worker. */
		LWLockReleaseAll();
		if (CurrentResourceOwner != NULL)
		{
			ResourceOwnerRelease(CurrentResourceOwner,
								 RESOURCE_RELEASE_BEFORE_LOCKS, false, false);
			ResourceOwnerRelease(CurrentResourceOwner,
								 RESOURCE_RELEASE_LOCKS, false, false);
			ResourceOwnerRelease(CurrentResourceOwner,
								 RESOURCE_RELEASE_AFTER_LOCKS, false, false);
		}

		/*
		 * Release any hash_seq_search scan and temp files abandoned when the
		 * error interrupted a cycle mid-scan.  Missing the hash-table reset here
		 * leaks dynahash scan registrations across errors until hash_seq_init
		 * itself fails, permanently wedging the service (it could then never
		 * scan the channel map to deliver to consumers).
		 */
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		MemoryContextSwitchTo(service_ctx);
		FlushErrorState();
		MemoryContextResetAndDeleteChildren(service_ctx);

		RESUME_INTERRUPTS();

		/* Do not spin on a persistent error. */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR). */
	PG_exception_stack = &local_sigjmp_buf;

	while (!anser_service_got_sigterm)
	{
		if (anser_service_got_sighup)
		{
			anser_service_got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * Run this service's data-path pass, then the shared orphan sweep.
		 * The gather service drains producer submissions and enforces the
		 * produce timeout; the send service delivers ready/cancelled channels
		 * to waiting consumers.  The timed wakeup bounds how long a stale
		 * COLLECTING channel or a dead-backend slot lingers between latches.
		 */
		if (gather_service)
			AnserGatherServiceCycle();
		else
			AnserSendServiceCycle();

		AnserServiceMaintenance();
		AnserWaitServiceLatch(gather_service, ANSER_SERVICE_WAKEUP_INTERVAL_MS);

		/* Reclaim any transient allocations made during this cycle. */
		MemoryContextReset(service_ctx);
	}

	PG_exception_stack = NULL;
	AnserDetachServiceLatch(gather_service);

	proc_exit(0);
}

static void
AnserServiceSigHup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	anser_service_got_sighup = true;
	AnserWakeServiceLatch(true);
	AnserWakeServiceLatch(false);
	SetLatch(MyLatch);
	errno = save_errno;
}

static void
AnserServiceSigTerm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	anser_service_got_sigterm = true;
	AnserWakeServiceLatch(true);
	AnserWakeServiceLatch(false);
	SetLatch(MyLatch);
	errno = save_errno;
}

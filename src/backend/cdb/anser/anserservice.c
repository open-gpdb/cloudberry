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
 *	  Coordinator-local Anser background service skeletons.
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
#include "storage/ipc.h"
#include "storage/latch.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
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
	pqsignal(SIGHUP, AnserServiceSigHup);
	pqsignal(SIGTERM, AnserServiceSigTerm);
	BackgroundWorkerUnblockSignals();

	init_ps_display(service_name);
	ereport(LOG,
			(errmsg_internal("%s started", service_name)));

	AnserAttachServiceLatch(gather_service);
	while (!anser_service_got_sigterm)
	{
		if (anser_service_got_sighup)
		{
			anser_service_got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		AnserServiceMaintenance();
		AnserWaitServiceLatch(gather_service, 1000L);
	}
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

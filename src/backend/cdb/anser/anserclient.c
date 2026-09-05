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
 * anserclient.c
 *	  libpq client helpers for the Anser network transport.
 *
 * A remote producer/consumer (running on a segment, Gp_role == GP_ROLE_EXECUTE)
 * cannot touch the coordinator-resident channel map directly.  Instead it opens
 * an ordinary libpq connection to the QD -- discovered from gp_qd_hostname /
 * gp_qd_port, which the dispatcher injects into every QE -- and calls the
 * gp_anser_* built-in functions.  When the QD supplied a session token (carried
 * in the plan), the connection authenticates with it via the gp_anser_conn
 * startup marker, bypassing pg_hba (the parallel-retrieve-cursor model);
 * otherwise authentication falls back to pg_hba.  Encryption and connection
 * lifecycle are inherited from libpq; these helpers are the client edges only.
 *
 * Everything here is fail-open: a broken connection degrades to unfiltered
 * execution, never to a wrong result or an error propagated into the query.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/anser/anserclient.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq-fe.h"

#include "cdb/anser.h"
#include "cdb/anserclient.h"
#include "cdb/cdbvars.h"
#include "commands/dbcommands.h"
#include "libpq/libpq-be.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/wait_event.h"

/* Enough for the decimal form of any int32 argument. */
#define ANSER_INT_STRLEN	12

static PGconn *anser_client_connect(const char *token);
static int	anser_client_exec_bool(PGconn *conn, const char *sql, int nparams,
								   const char *const *values, const int *lengths,
								   const int *formats);
static int	anser_client_producer_begin(PGconn *conn,
										const AnserChannelKey *key,
										uint32 expected_producers);
static int	anser_client_publish_part(PGconn *conn,
									  const AnserChannelKey *key,
									  const void *payload, Size payload_len,
									  bool cancelled);
static PGresult *anser_client_wait_result(PGconn *conn, const char *sql,
										  int nparams,
										  const char *const *values,
										  const int *lengths,
										  const int *formats,
										  int result_format);

/*
 * Open a libpq connection to the QD postmaster, reusing the query's database and
 * user.  Returns NULL (never raises) on any failure so callers can fail open.
 *
 * When a session token is given, the connection carries the gp_anser_conn=true
 * startup marker and the token as password, which the QD authenticates against
 * its session-token hash before pg_hba is consulted (see
 * anser_conn_authentication in libpq/auth.c) -- so no pg_hba entry for the
 * segment hosts is needed.  Without a token the connection goes through
 * ordinary pg_hba-driven authentication.
 */
static PGconn *
anser_client_connect(const char *token)
{
	const char *keywords[10];
	const char *values[10];
	int			n = 0;
	char		portstr[ANSER_INT_STRLEN];
	const char *dbname;
	const char *user;
	PGconn	   *conn;

	if (qdHostname == NULL || qdHostname[0] == '\0' || qdPostmasterPort <= 0)
		return NULL;

	snprintf(portstr, sizeof(portstr), "%d", qdPostmasterPort);

	if (MyProcPort != NULL && MyProcPort->database_name != NULL)
		dbname = MyProcPort->database_name;
	else if (OidIsValid(MyDatabaseId))
		dbname = get_database_name(MyDatabaseId);
	else
		dbname = NULL;

	if (MyProcPort != NULL && MyProcPort->user_name != NULL)
		user = MyProcPort->user_name;
	else
		user = GetUserNameFromId(GetUserId(), true);

	if (dbname == NULL || user == NULL)
		return NULL;

	keywords[n] = "host";
	values[n] = qdHostname;
	n++;
	keywords[n] = "port";
	values[n] = portstr;
	n++;
	keywords[n] = "dbname";
	values[n] = dbname;
	n++;
	keywords[n] = "user";
	values[n] = user;
	n++;
	keywords[n] = "client_encoding";
	values[n] = GetDatabaseEncodingName();
	n++;
	keywords[n] = "connect_timeout";
	values[n] = "10";
	n++;
	if (token != NULL && token[0] != '\0')
	{
		keywords[n] = "password";
		values[n] = token;
		n++;
		keywords[n] = "options";
		values[n] = "-c gp_anser_conn=true";
		n++;
	}
	keywords[n] = "application_name";
	values[n] = "anser_rf";
	n++;
	keywords[n] = NULL;
	values[n] = NULL;

	conn = PQconnectdbParams(keywords, values, false);
	if (conn == NULL)
		return NULL;
	if (PQstatus(conn) != CONNECTION_OK)
	{
		PQfinish(conn);
		return NULL;
	}

	return conn;
}

/*
 * Run a bool-returning gp_anser_* function.  Returns 1 (true), 0 (false), or -1
 * on any protocol error.
 */
static int
anser_client_exec_bool(PGconn *conn, const char *sql, int nparams,
					   const char *const *values, const int *lengths,
					   const int *formats)
{
	PGresult   *res;
	int			ret;

	res = PQexecParams(conn, sql, nparams, NULL, values, lengths, formats, 0);
	if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1 || PQnfields(res) != 1)
	{
		if (res != NULL)
			PQclear(res);
		return -1;
	}

	if (PQgetisnull(res, 0, 0))
		ret = 0;
	else
		ret = (strcmp(PQgetvalue(res, 0, 0), "t") == 0) ? 1 : 0;

	PQclear(res);
	return ret;
}

static int
anser_client_producer_begin(PGconn *conn, const AnserChannelKey *key,
							uint32 expected_producers)
{
	const char *values[5];
	char		ssid[ANSER_INT_STRLEN];
	char		ccnt[ANSER_INT_STRLEN];
	char		condid[ANSER_INT_STRLEN];
	char		expected[ANSER_INT_STRLEN];

	snprintf(ssid, sizeof(ssid), "%d", key->gp_session_id);
	snprintf(ccnt, sizeof(ccnt), "%d", key->gp_command_count);
	snprintf(condid, sizeof(condid), "%d", (int) key->condition_id);
	snprintf(expected, sizeof(expected), "%u", expected_producers);

	values[0] = ssid;
	values[1] = ccnt;
	values[2] = condid;
	values[3] = key->condition_key;
	values[4] = expected;

	return anser_client_exec_bool(conn,
								  "SELECT gp_anser_producer_begin($1::int4, $2::int4, $3::int4, $4::text, $5::int4)",
								  5, values, NULL, NULL);
}

static int
anser_client_publish_part(PGconn *conn, const AnserChannelKey *key,
						  const void *payload, Size payload_len, bool cancelled)
{
	const char *values[6];
	int			lengths[6];
	int			formats[6];
	char		ssid[ANSER_INT_STRLEN];
	char		ccnt[ANSER_INT_STRLEN];
	char		condid[ANSER_INT_STRLEN];

	snprintf(ssid, sizeof(ssid), "%d", key->gp_session_id);
	snprintf(ccnt, sizeof(ccnt), "%d", key->gp_command_count);
	snprintf(condid, sizeof(condid), "%d", (int) key->condition_id);

	memset(lengths, 0, sizeof(lengths));
	memset(formats, 0, sizeof(formats));

	values[0] = ssid;
	values[1] = ccnt;
	values[2] = condid;
	values[3] = key->condition_key;

	/* $5 payload: raw bytea in binary format (empty when cancelling). */
	if (!cancelled && payload != NULL && payload_len > 0)
	{
		values[4] = (const char *) payload;
		lengths[4] = (int) payload_len;
	}
	else
	{
		values[4] = "";
		lengths[4] = 0;
	}
	formats[4] = 1;

	values[5] = cancelled ? "t" : "f";

	return anser_client_exec_bool(conn,
								  "SELECT gp_anser_publish($1::int4, $2::int4, $3::int4, $4::text, $5::bytea, $6::bool)",
								  6, values, lengths, formats);
}

bool
AnserClientPublish(const AnserChannelKey *channel_key,
				   uint32 expected_producers, const void *payload,
				   Size payload_len, bool cancelled, const char *token)
{
	PGconn	   *conn;
	bool		ok = false;

	if (channel_key == NULL)
		return false;

	conn = anser_client_connect(token);
	if (conn == NULL)
		return false;			/* fail open */

	if (anser_client_producer_begin(conn, channel_key, expected_producers) == 1 &&
		anser_client_publish_part(conn, channel_key, payload, payload_len,
								  cancelled) == 1)
		ok = true;
	else if (!cancelled)
	{
		/*
		 * Something went wrong mid-publish.  Best-effort cancel so the dataset
		 * dies cleanly rather than leaving consumers to time out.
		 */
		(void) anser_client_publish_part(conn, channel_key, NULL, 0, true);
	}

	PQfinish(conn);
	return ok;
}

/*
 * Issue a query and block interruptibly for its result.  Unlike PQexecParams,
 * this pumps the connection through WaitLatchOrSocket so the calling backend
 * still honors query cancellation while the coordinator holds the consumer.  On
 * interrupt we forward a cancel to the QD backend and re-raise, so the blocked
 * gp_anser_consume_wait there unwinds and its wait slot is reaped.
 */
static PGresult *
anser_client_wait_result(PGconn *conn, const char *sql, int nparams,
						 const char *const *values, const int *lengths,
						 const int *formats, int result_format)
{
	PGresult   *res = NULL;
	PGresult   *tmp;
	bool		failed = false;

	if (!PQsendQueryParams(conn, sql, nparams, NULL, values, lengths, formats,
						   result_format))
		return NULL;

	/* Never "return" from inside PG_TRY: flag failures and handle them after. */
	PG_TRY();
	{
		for (;;)
		{
			CHECK_FOR_INTERRUPTS();

			if (!PQconsumeInput(conn))
			{
				failed = true;
				break;
			}

			if (!PQisBusy(conn))
				break;

			(void) WaitLatchOrSocket(MyLatch,
									 WL_LATCH_SET | WL_SOCKET_READABLE |
									 WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
									 PQsocket(conn), 1000L,
									 PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
		}
	}
	PG_CATCH();
	{
		PGcancel   *cancel = PQgetCancel(conn);

		if (cancel != NULL)
		{
			char		errbuf[256];

			(void) PQcancel(cancel, errbuf, sizeof(errbuf));
			PQfreeCancel(cancel);
		}
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (failed)
		return NULL;

	res = PQgetResult(conn);
	/* Drain any trailing results so the connection is reusable/closable. */
	while ((tmp = PQgetResult(conn)) != NULL)
		PQclear(tmp);

	return res;
}

bool
AnserClientConsumeWait(const AnserChannelKey *channel_key, void **payload,
					   Size *payload_len, bool *cancelled, const char *token)
{
	PGconn	   *conn;
	PGresult   *res;
	const char *values[4];
	char		ssid[ANSER_INT_STRLEN];
	char		ccnt[ANSER_INT_STRLEN];
	char		condid[ANSER_INT_STRLEN];
	bool		ok = false;

	if (payload != NULL)
		*payload = NULL;
	if (payload_len != NULL)
		*payload_len = 0;
	if (cancelled != NULL)
		*cancelled = false;

	if (channel_key == NULL)
		return false;

	conn = anser_client_connect(token);
	if (conn == NULL)
	{
		if (cancelled != NULL)
			*cancelled = true;
		return false;
	}

	snprintf(ssid, sizeof(ssid), "%d", channel_key->gp_session_id);
	snprintf(ccnt, sizeof(ccnt), "%d", channel_key->gp_command_count);
	snprintf(condid, sizeof(condid), "%d", (int) channel_key->condition_id);
	values[0] = ssid;
	values[1] = ccnt;
	values[2] = condid;
	values[3] = channel_key->condition_key;

	PG_TRY();
	{
		res = anser_client_wait_result(conn,
									   "SELECT gp_anser_consume_wait($1::int4, $2::int4, $3::int4, $4::text)",
									   4, values, NULL, NULL, 1);
	}
	PG_CATCH();
	{
		PQfinish(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1 || PQnfields(res) != 1)
	{
		if (res != NULL)
			PQclear(res);
		PQfinish(conn);
		if (cancelled != NULL)
			*cancelled = true;
		return false;
	}

	if (PQgetisnull(res, 0, 0))
	{
		if (cancelled != NULL)
			*cancelled = true;
	}
	else
	{
		int			len = PQgetlength(res, 0, 0);
		char	   *val = PQgetvalue(res, 0, 0);
		void	   *buf = NULL;

		if (len > 0)
		{
			buf = palloc(len);
			memcpy(buf, val, len);
		}
		if (payload != NULL)
			*payload = buf;
		if (payload_len != NULL)
			*payload_len = (Size) len;
		ok = true;
	}

	PQclear(res);
	PQfinish(conn);
	return ok;
}

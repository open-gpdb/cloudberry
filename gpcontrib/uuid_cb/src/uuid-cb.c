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
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * uuid-cb.c
 *
 * IDENTIFICATION
 *	  gpcontrib/uuid_cb/src/uuid-cb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "uid.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(uuid_cb_generate);
PG_FUNCTION_INFO_V1(uuid_cb_valid);

void		_PG_init(void);
void		_PG_fini(void);

#define UUID_LEN	36
#define CB_UUID_LEN	38

Datum
uuid_cb_generate(PG_FUNCTION_ARGS)
{
	char buf[CB_UUID_LEN + 1];
	if (!uid_create(buf)) {
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					errmsg("Could not generate CB UUID")));
	}
	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf, CB_UUID_LEN));
}

Datum
uuid_cb_valid(PG_FUNCTION_ARGS)
{
	char 	*uuid_cb_str;
	char	uuid_str[UUID_LEN + 1];
	uuid_cb_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
	if (!uuid_cb_str || strlen(uuid_cb_str) != CB_UUID_LEN) {
		PG_RETURN_BOOL(false);
	}
	strncpy(uuid_str, uuid_cb_str, UUID_LEN);
	uuid_str[UUID_LEN] = 0;
	PG_RETURN_BOOL(calc_ctrl(uuid_str) == uuid_cb_str[CB_UUID_LEN - 1]);
}

void
_PG_init(void)
{
	if (!uid_init()) {
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					errmsg("Could not initialize CB UUID generator")));
	}
}

void
_PG_fini(void)
{
	uid_deinit();
}
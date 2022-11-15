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
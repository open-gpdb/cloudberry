# src/bin/pg_controldata/nls.mk
CATALOG_NAME     = pg_controldata
<<<<<<< HEAD
AVAIL_LANGUAGES  = cs de el es fr it ja ko ru sv tr uk zh_CN
=======
>>>>>>> REL_16_9
GETTEXT_FILES    = pg_controldata.c ../../common/controldata_utils.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)

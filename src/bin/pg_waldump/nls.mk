# src/bin/pg_waldump/nls.mk
CATALOG_NAME     = pg_waldump
<<<<<<< HEAD
AVAIL_LANGUAGES  = cs de el es fr ja ko ru sv tr uk zh_CN
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) pg_waldump.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) fatal_error
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) fatal_error:1:c-format
=======
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   pg_waldump.c \
                   xlogreader.c \
                   xlogstats.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) report_invalid_record:2
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
    report_invalid_record:2:c-format
>>>>>>> REL_16_9

# src/bin/pg_rewind/nls.mk
CATALOG_NAME     = pg_rewind
<<<<<<< HEAD
AVAIL_LANGUAGES  = cs de el es fr ja ru sv uk zh_CN
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) datapagemap.c file_ops.c filemap.c libpq_source.c local_source.c parsexlog.c pg_rewind.c timeline.c xlogreader.c ../../common/fe_memutils.c ../../common/restricted_token.c ../../fe_utils/archive.c ../../fe_utils/recovery_gen.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) pg_fatal report_invalid_record:2
=======
GETTEXT_FILES    = $(FRONTEND_COMMON_GETTEXT_FILES) \
                   datapagemap.c \
                   file_ops.c \
                   filemap.c \
                   libpq_source.c \
                   local_source.c \
                   parsexlog.c \
                   pg_rewind.c \
                   timeline.c \
                   xlogreader.c \
                   ../../common/fe_memutils.c \
                   ../../common/percentrepl.c \
                   ../../common/restricted_token.c \
                   ../../fe_utils/archive.c \
                   ../../fe_utils/recovery_gen.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS) report_invalid_record:2
>>>>>>> REL_16_9
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS) \
    report_invalid_record:2:c-format

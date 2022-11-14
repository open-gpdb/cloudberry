EXTENSION = uuid-cb
EXTVERSION = $(shell grep default_version $(EXTENSION).control | \
			   sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

DATA = $(wildcard sql/*--*.sql)
REGRESS = uuid_cb
REGRESS_OPTS = --inputdir=test/
OBJS = src/uuid-cb.o src/uuid.o src/uid.o

MODULE_big = uuid-cb

ifeq ($(OS),Windows_NT)
		SHLIB_LINK += -lcrypt32
endif

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

EXTENSION = uuid-cb
EXTVERSION = $(shell grep default_version $(EXTENSION).control | \
			   sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

DATA = $(wildcard sql/*--*.sql)
REGRESS = uuid_cb
REGRESS_OPTS = --inputdir=test/
OBJS = src/uuid-cb.o src/uuid.o src/uid.o

MODULE_big = uuid-cb

ifdef USE_PGXS
	PG_CONFIG       = pg_config
	PGXS            := $(shell $(PG_CONFIG) --pgxs)
	include $(PGXS)
else
	top_builddir = ../..
	include $(top_builddir)/src/Makefile.global
	include $(top_srcdir)/contrib/contrib-global.mk
endif

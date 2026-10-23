#-------------------------------------------------------------------------
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#  http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# Makefile for the robust_predicate_transfer extension.
#
# IDENTIFICATION
#    gpcontrib/robust_predicate_transfer/Makefile
#
#-------------------------------------------------------------------------

MODULE_big = robust_predicate_transfer
OBJS = \
	$(WIN32RES) \
	src/rptgraph.o \
	src/rptinit.o \
	src/rptplan.o \
	src/rptregion.o

PGFILEDESC = "robust_predicate_transfer - RPT+ transfer plans over Anser"

EXTENSION = robust_predicate_transfer
DATA = robust_predicate_transfer--1.0.sql

REGRESS = rpt_regions rpt_graph

# Anser is a sibling module, not a linked library: it is preloaded and dfmgr.c
# loads modules RTLD_GLOBAL, so the AnserXxx entry points resolve at load time.
# That is what the PGDLLEXPORT markers on Anser's public headers are for.
PG_CPPFLAGS = -I$(srcdir)/include -I$(srcdir)/../anser/include

# C++ module; LLVM bitcode is not generated for it (as gpcloud).
with_llvm = no

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = gpcontrib/robust_predicate_transfer
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# The analysis needs anser's library installed and preloaded: our module
# resolves AnserIsRuntimeFilterScan from it at load time (RTLD_NOW), and
# load order must be anser first.  Mirrors gpcontrib/anser's preload
# machinery.
installcheck: preload-rpt

.PHONY: preload-rpt
preload-rpt:
	@if [ -z "$$COORDINATOR_DATA_DIRECTORY$$MASTER_DATA_DIRECTORY" ]; then \
	    echo "ERROR: COORDINATOR_DATA_DIRECTORY (or MASTER_DATA_DIRECTORY) is not set;" >&2; \
	    echo "       source cloudberry-env.sh and gpdemo-env.sh before running installcheck." >&2; \
	    exit 1; \
	fi
	@current=`psql -d postgres -At -c "SHOW shared_preload_libraries;"` || { \
	    echo "ERROR: cannot read shared_preload_libraries; is the cluster up?" >&2; \
	    exit 1; \
	}; \
	current=`echo "$$current" | tr -d ' '`; \
	case ",$$current," in \
	  *,anser,*robust_predicate_transfer,*) \
	    echo "==> shared_preload_libraries already carries anser + robust_predicate_transfer" ;; \
	  *) \
	    without=`echo "$$current" | sed -e 's/anser//g' -e 's/robust_predicate_transfer//g' -e 's/,,*/,/g' -e 's/^,//' -e 's/,$$//'`; \
	    if [ -z "$$without" ]; then new="anser,robust_predicate_transfer"; else new="$$without,anser,robust_predicate_transfer"; fi; \
	    echo "==> Setting shared_preload_libraries to $$new"; \
	    gpconfig -c shared_preload_libraries -v "'$$new'" --skipvalidation || exit 1; \
	    gpstop -ra || exit 1 ;; \
	esac

# CI runs a single make target per test config, so offer one target that
# installs the extension (and Anser, which we load-resolve against), arms the
# cluster, and runs the tests.
.PHONY: installcheck-with-install
installcheck-with-install:
	$(MAKE) -C ../anser install
	$(MAKE) install
	$(MAKE) installcheck

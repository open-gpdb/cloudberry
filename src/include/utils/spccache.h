/*-------------------------------------------------------------------------
 *
 * spccache.h
 *	  Tablespace cache.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/spccache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPCCACHE_H
#define SPCCACHE_H

<<<<<<< HEAD
#include "commands/tablespace.h"

typedef struct
{
	Oid			oid;			/* lookup key - must be first */
	TableSpaceOpts *opts;		/* options, or NULL if none */
} TableSpaceCacheEntry;

void		get_tablespace_page_costs(Oid spcid, float8 *spc_random_page_cost,
=======
extern void get_tablespace_page_costs(Oid spcid, float8 *spc_random_page_cost,
>>>>>>> REL_16_9
									  float8 *spc_seq_page_cost);
extern int	get_tablespace_io_concurrency(Oid spcid);
extern int	get_tablespace_maintenance_io_concurrency(Oid spcid);

extern TableSpaceCacheEntry *get_tablespace(Oid spcid);

#endif							/* SPCCACHE_H */

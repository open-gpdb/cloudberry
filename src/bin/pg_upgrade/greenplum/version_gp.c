/*
 *	version_gp.c
 *
 *	Greenplum version-specific routines for upgrades
 *
 *	Copyright (c) 2016-Present VMware, Inc. or its affiliates
 *	contrib/pg_upgrade/version_gp.c
 */
#include "postgres_fe.h"

#include "pg_upgrade_greenplum.h"

#include "access/transam.h"
#include "fe_utils/string_utils.h"

#define NUMERIC_ALLOC 100

/*
 *	check_hash_partition_usage()
 *	8.3 -> 8.4
 *
 *	Hash partitioning was never officially supported in GPDB5 and was removed
 *	in GPDB6, but better check just in case someone has found the hidden GUC
 *	and used them anyway.
 *
 *	The hash algorithm was changed in 8.4, so upgrading is impossible anyway.
 *	This is basically the same problem as with hash indexes in PostgreSQL.
 */
void
check_hash_partition_usage(void)
{
	int				dbnum;
	FILE		   *script = NULL;
	bool			found = false;
	char			output_path[MAXPGPATH];

	/* Merge with PostgreSQL v11 introduced hash partitioning again. */
	if (GET_MAJOR_VERSION(old_cluster.major_version) >= 1100)
		return;

	prep_status("Checking for hash partitioned tables");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir, "hash_partitioned_tables.txt");

	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &old_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&old_cluster, active_db->db_name);

		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM pg_catalog.pg_partition p, pg_catalog.pg_class c, pg_catalog.pg_namespace n "
								"WHERE p.parrelid = c.oid AND c.relnamespace = n.oid "
								"AND parkind = 'h'");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen(output_path, "w")) == NULL)
				pg_log(PG_FATAL, "Could not create necessary file:  %s\n", output_path);
			if (!db_used)
			{
				fprintf(script, "Database:  %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (found)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal\n");
		gp_fatal_log(
			   "| Your installation contains hash partitioned tables.\n"
			   "| Upgrading hash partitioned tables is not supported,\n"
			   "| so this cluster cannot currently be upgraded.  You\n"
			   "| can remove the problem tables and restart the\n"
			   "| migration.  A list of the problem tables is in the\n"
			   "| file:\n"
			   "| \t%s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 * old_GPDB5_check_for_unsupported_distribution_key_data_types()
 *
 *	abstime, reltime, tinterval, money and anyarray datatypes don't have hash opclasses
 *	in GPDB 6, so they are not supported as distribution keys anymore.
 */
void
old_GPDB5_check_for_unsupported_distribution_key_data_types(void)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for abstime, reltime, tinterval user data types");

	snprintf(output_path, sizeof(output_path), "tables_using_abstime_reltime_tinterval.txt");

	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname,
					i_attname;
		DbInfo	   *active_db = &old_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&old_cluster, active_db->db_name);

		res = executeQueryOrDie(conn,
								"SELECT nspname, relname, attname "
								"FROM   pg_catalog.pg_class c, "
								"       pg_catalog.pg_namespace n, "
								"       pg_catalog.pg_attribute a, "
								"       gp_distribution_policy p "
								"WHERE  c.oid = a.attrelid AND "
								"       c.oid = p.localoid AND "
								"       a.atttypid in ('pg_catalog.abstime'::regtype, "
								"                      'pg_catalog.reltime'::regtype, "
								"                      'pg_catalog.tinterval'::regtype, "
								"                      'pg_catalog.money'::regtype, "
								"                      'pg_catalog.anyarray'::regtype) AND "
								"       attnum = any (p.attrnums) AND "
								"       c.relnamespace = n.oid AND "
		/* exclude possible orphaned temp tables */
								"  		n.nspname !~ '^pg_temp_'");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		i_attname = PQfnumber(res, "attname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("Could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname),
					PQgetvalue(res, rowno, i_attname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains a user table, that uses a 'abstime',\n"
				 "'reltime', 'tinterval', 'money' or 'anyarray' type as a distribution key column. Using\n"
				 "these datatypes as distribution keys is no longer supported. You can use\n"
				 "ALTER TABLE ... SET DISTRIBUTED RANDOMLY to change the distribution keys,\n"
				 "and restart the upgrade.  A list of the problem columns is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 * old_GPDB6_check_for_unsupported_sha256_password_hashes()
 *
 *  Support for password_hash_algorithm='sha-256' was removed in GPDB 7. Check if
 *  any roles have SHA-256 password hashes.
 */
void
old_GPDB6_check_for_unsupported_sha256_password_hashes(void)
{
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for SHA-256 hashed passwords");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir, "roles_using_sha256_passwords.txt");

	/* It's enough to check this in one database, pg_authid is a shared catalog. */
	{
		PGresult   *res;
		int			ntups;
		int			rowno;
		int			i_rolname;
		DbInfo	   *active_db = &old_cluster.dbarr.dbs[0];
		PGconn	   *conn = connectToServer(&old_cluster, active_db->db_name);

		res = executeQueryOrDie(conn,
								"SELECT rolname FROM pg_catalog.pg_authid "
								"WHERE rolpassword LIKE 'sha256%%'");

		ntups = PQntuples(res);
		i_rolname = PQfnumber(res, "rolname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("Could not open file \"%s\": %s\n",
						 output_path, strerror(errno));
			fprintf(script, "  %s\n",
					PQgetvalue(res, rowno, i_rolname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		gp_fatal_log(
				 "| Your installation contains roles with SHA-256 hashed passwords. Using\n"
				 "| SHA-256 for password hashes is no longer supported. You can use\n"
				 "| ALTER ROLE <role name> WITH PASSWORD NULL as superuser to clear passwords,\n"
				 "| and restart the upgrade.  A list of the problem roles is in the file:\n"
				 "|    %s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 * new_gpdb_invalidate_indexes()
 *
 * pg_upgrade can only carry btree indexes over unchanged.  Every other access
 * method has an on-disk format that differs between the old GPDB cluster and
 * the new Cloudberry cluster: bitmap is Greenplum-specific and unmigratable,
 * and gin/gist/spgist/hash/brin all changed format across the underlying
 * PostgreSQL major versions (GPDB6 is 9.4-based, Cloudberry is 14-based).
 * Their relfilenodes are transferred verbatim, so reading them on the new
 * cluster yields garbage and crashes the backend.
 *
 * Mark every such index as neither ready nor valid.  Clearing indisready is
 * what actually protects us: vac_open_indexes() (and hence autovacuum) skips
 * indexes that are not indisready, whereas indisvalid=false indexes are still
 * vacuumed.  Without this, the first autovacuum worker to touch a carried-over
 * gin/bitmap index segfaults.  A reindex script is written so the user can
 * rebuild the indexes once the upgrade has finished.
 */
void
new_gpdb_invalidate_indexes(void)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Invalidating non-btree indexes in new cluster");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir, "reindex_indexes.sql");

	for (dbnum = 0; dbnum < new_cluster.dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &new_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&new_cluster, active_db->db_name);

		/*
		 * GPDB doesn't allow hacking the catalogs without setting
		 * allow_system_table_mods first.
		 */
		PQclear(executeQueryOrDie(conn, "set allow_system_table_mods=true"));

		/* find user indexes whose access method is not btree */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM   pg_catalog.pg_class c, "
								"       pg_catalog.pg_index i, "
								"       pg_catalog.pg_am a, "
								"       pg_catalog.pg_namespace n "
								"WHERE  i.indexrelid = c.oid AND "
								"       c.relam = a.oid AND "
								"       c.relnamespace = n.oid AND "
								"       a.amname <> 'btree' AND "
								"       c.oid >= %u",
								FirstNormalObjectId);

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n", output_path,
						 strerror(errno));
			if (!db_used)
			{
				PQExpBufferData connectbuf;

				initPQExpBuffer(&connectbuf);
				appendPsqlMetaConnect(&connectbuf, active_db->db_name);
				fputs(connectbuf.data, script);
				termPQExpBuffer(&connectbuf);
				db_used = true;
			}
			fprintf(script, "REINDEX INDEX %s.%s;\n",
					quote_identifier(PQgetvalue(res, rowno, i_nspname)),
					quote_identifier(PQgetvalue(res, rowno, i_relname)));
		}

		PQclear(res);

		if (!user_opts.check)
		{
			/*
			 * Clearing indisready keeps (auto)vacuum from opening the
			 * incompatible relfilenode; clearing indisvalid keeps the planner
			 * from using it.  REINDEX restores both.
			 */
			PQclear(executeQueryOrDie(conn,
									  "UPDATE pg_catalog.pg_index i "
									  "SET    indisready = false, "
									  "       indisvalid = false "
									  "FROM   pg_catalog.pg_class c, "
									  "       pg_catalog.pg_am a "
									  "WHERE  i.indexrelid = c.oid AND "
									  "       c.relam = a.oid AND "
									  "       a.amname <> 'btree' AND "
									  "       c.oid >= %u",
									  FirstNormalObjectId));
		}

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		pg_log(PG_WARNING, "\n"
			   "Your installation contains indexes using access methods other than\n"
			   "btree (for example bitmap or gin).  These indexes have on-disk formats\n"
			   "that are incompatible between your old and new clusters, so they have\n"
			   "been marked invalid and must be rebuilt with the REINDEX command.  The\n"
			   "file\n"
			   "    %s\n"
			   "when executed by psql by the database superuser will recreate all\n"
			   "invalid indexes; until then, none of these indexes will be used.\n\n",
			   output_path);
	}
	else
		check_ok();
}

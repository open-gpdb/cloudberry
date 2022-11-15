## Overview
UUID-CB PostgreSQL/Greenplum extension provides functions to generate text UUIDs in format required by Russian Central Bank. The only difference from commonly used UUID format is that CB UUIDs have additional hexadecimal character for checksum:

	UUID:    f6553a80-642d-11ed-854e-09a55775d327
	UUID-CB: f6553a80-642d-11ed-854e-09a55775d327-9
	
Here "9" is the checksum.

## Building
To build the extension you need those already installed:

* GNU make
* GCC
* PostgreSQL or Greenplum

In addition you need to include greenplum binaries into PATH variable:

```
PATH="${GPHOME}/bin:${PATH}"
```
This will allow build and install instructions to find pg_config, which provides the rest of information, necessary for the build: library paths, include paths, etc.

Once it is done you can proceed with `make && make install` and it should install uuid-cb into proper location for your PG/GP installation.

### Running regression tests
To make sure the extension works well, you can start your database server:

1.  Set environment variables so that tests can find the server: `export PGPORT=your gp port`
2.  Run `make installcheck` from uuid-cb root directory

If everything is configured correctly, you should see one test passing.

## Usage
To use the extension first, you need to install it as described above. Then you need to load the extension if it not loaded yet: 

```sql
CREATE EXTENSION IF NOT EXISTS "uuid-cb";
```

After it`s done you can use it like this:

```sql
SELECT uuid_cb_generate(); -- to generate a uuid
SELECT uuid_cb_valid(uuid_cb_generate()); -- to make sure the string is a valid CB UUID 
```
To create a table that uses CB UUIDs it is recommended to make the uuid column a primary key and distribute the table by it. Also it is recommended to add a check constraint on this column using `uuid_cb_valid`:

```sql
CREATE TABLE uid_tbl (uid CHAR(38) NOT NULL DEFAULT uuid_cb_generate() \
CHECK (uuid_cb_valid(uid) = true), data INTEGER NOT NULL, \
PRIMARY KEY (uid)) DISTRIBUTED BY(uid);
```
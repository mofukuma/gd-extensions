/**************************************************************************/
/*  sqlite.c                                                              */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

// SQLite amalgamationをgd-cli向けの有界設定で1回だけcompileする。

#define SQLITE_THREADSAFE 1
#define SQLITE_DQS 0
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_MAX_MEMORY 268435456
#define SQLITE_MAX_LENGTH 16777216
#define SQLITE_MAX_SQL_LENGTH 1048576
#define SQLITE_MAX_VARIABLE_NUMBER 999

#include "sqlite3.c"

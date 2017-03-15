//
// Copyright © 2017 by Yuri Victorovich. All rights reserved.
//

#include "common.h"
#include "sqlite-interface.h"
#include "database.h"
#include "util.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

//#define USE_BLOB_CACHE    // blob cache can't be used until the SQLite bug is fixed

// macros
#define CK_ERROR(stmt...) \
  (SQLITE_OK != (rc = stmt))
#define LOG(fmt...) //utilLog(__FUNCTION__, "Db", fmt);

// database objects
static sqlite3 *db = NULL;
static DbLockCb dbLockCb = NULL;
static DbUnlockCb dbUnlockCb = NULL;
static void *dbLockUserData = NULL;
static uint8_t dbInMemory = 0;
#if defined(USE_BLOB_CACHE)
#define USE_SQLITE_WORKAROUND // this workaround avoids rowid collisions, but doesn't help in the case of concurrently received messages
static sqlite3_blob *blobCache = NULL;
static int64_t blobCacheRowid = 0;
#endif
static char dbName[64];

// prepared statements
static sqlite3_stmt *stmtInsertFragmentedDataInbound = NULL;
static sqlite3_stmt *stmtInsertFragmentedMetaInbound = NULL;
static sqlite3_stmt *stmtSelectRowidFromFragmentedMeta = NULL;
static sqlite3_stmt *stmtInsertFragmentedDataOutbound = NULL;
static sqlite3_stmt *stmtInsertFragmentedMetaOutbound = NULL;
static sqlite3_stmt *stmtUpdateFragmentedMeta = NULL;
static sqlite3_stmt *stmtSelectFragmentedInboundDone = NULL;
static sqlite3_stmt *stmtSelectFragmentedOutboundPending = NULL;
static sqlite3_stmt *stmtDeleteFragmentedData = NULL;

// internal declarations

static void* dbLock();
static void dbUnlock(void *lock);
static void initDb();
static void execSql(const char *sql);
static void createSchema();
static void readDbName(char *name);
static uint64_t getFragmentsDataRowid(uint32_t friend_number, uint64_t id);
static void updateFragmentedMetaDone(uint64_t tm, uint32_t friend_number, uint64_t id);
static void deleteDataRecord(uint32_t friend_number, uint64_t id);
static sqlite3_stmt* prepareStatement(const char *sql);
static void destroyPreparedStatement(sqlite3_stmt **stmt);
static void destroyPreparedStatements();
static void prepare(sqlite3_stmt **pstmt, const char *sql);
static void bindInt(sqlite3_stmt *stmt, int n, int a);
static void bindInt64(sqlite3_stmt *stmt, int n, sqlite3_int64 a);
static void bindBlob(sqlite3_stmt *stmt, int n, const uint8_t *data, size_t size);
static void bind_Int_Int64(sqlite3_stmt *stmt, int a1, sqlite3_int64 a2);
static void bind_Int64_Int_Int64(sqlite3_stmt *stmt, sqlite3_int64 a1, int a2, sqlite3_int64 a3);
static void bind_Int_Int64_Blob_Int_Int(sqlite3_stmt *stmt,
                                        int a1, sqlite3_int64 a2, const uint8_t *a3data, size_t a3size,
                                        int a4, int a5);
static void bind_Int_Int64_Int64_Int_Int64(sqlite3_stmt *stmt,
                                           int a1, sqlite3_int64 a2, sqlite3_int64 a3, int a4,
                                           sqlite3_int64 a5);
static void bind_Int_Int_Int64_Int64_Int64_Int(sqlite3_stmt *stmt,
                                               int a1, int a2, sqlite3_int64 a3, sqlite3_int64 a4,
                                               sqlite3_int64 a5, int a6);
static void bind_Int_Int_Int64_Int64_Int64_Int_Int_Int64(sqlite3_stmt *stmt,
                                                         int a1, int a2, sqlite3_int64 a3, sqlite3_int64 a4,
                                                         sqlite3_int64 a5, int a6, int a7, sqlite3_int64 a8);
static void execPrepared(sqlite3_stmt *stmt);
static int execPreparedRowOrNot(sqlite3_stmt *stmt);
static int execPreparedInt64(sqlite3_stmt *stmt, int iCol, int64_t *value);
static int execPreparedText(sqlite3_stmt *stmt, int iCol, unsigned char *value);
static void resetStmt(sqlite3_stmt *stmt);
static void err(int rc, const char *op);
static void errSql(int rc, const char *op, const char *sql);
static sqlite3_blob* openBlob(const char *table, const char *field, uint64_t rowid);
static void readBlob(sqlite3_blob *blob, uint8_t *data, unsigned length, unsigned off);
static void writeBlob(sqlite3_blob *blob, const uint8_t *data, unsigned length, unsigned off);
static void closeBlob(sqlite3_blob *blob);
#if defined(USE_BLOB_CACHE)
static void blobCacheCloseBlob();
#endif

// functions

FUNC_LOCAL void dbInitialize(sqlite3 *new_db, DbLockCb lockCb, DbUnlockCb unlockCb, void *user_data) {
  db = new_db;
  dbLockCb = lockCb;
  dbUnlockCb = unlockCb;
  dbLockUserData = user_data;
  initDb();
}

FUNC_LOCAL void dbInitializeInMemory() {
  int rc;
  if (CK_ERROR(sqlite3_open(":memory:", &db)))
    err(rc, "creating the in-memory database");
  dbInMemory = 1;
  initDb();
}

FUNC_LOCAL void dbUninitialize() {
#if defined(USE_BLOB_CACHE)
  if (blobCache)
    blobCacheCloseBlob();
#endif
  destroyPreparedStatements();
  if (dbInMemory) {
    int rc;
    if (CK_ERROR(sqlite3_close(db)))
      err(rc, "closing the in-memory database");
    dbInMemory = 0;
  }
  db = NULL;
  dbLockCb = NULL;
  dbUnlockCb = NULL;
  dbLockUserData = NULL;
  memset(dbName, 0, sizeof(dbName));
}

FUNC_LOCAL void dbInsertInboundFragment(void *tox_opaque,
                                        uint32_t friend_number, int type, uint64_t id,
                                        unsigned partNo, unsigned numParts, unsigned off, unsigned sz,
                                        const uint8_t *data, size_t length,
                                        uint64_t tm,
                                        DbMsgReadyCb msgReadyCb,
                                        void *user_data) {
  // Try inserting records while some other fragment can also be inserting it.
  // In case fragmented_meta exists but fragmented_data doesn't, this message is already finished
  // and fragments are considered duplicates and are ignored.

  LOG("part#%u off=%u sz=%u len=%u data=-->%*s<--", partNo, off, sz, (unsigned)length, (unsigned)length, (const char*)data)
  void *lock = dbLock();
  prepare(&stmtInsertFragmentedDataInbound,
    "INSERT INTO fragmented_data (friend_id, frags_id, message)"
    " SELECT ?, ?, zeroblob(?)"
    " WHERE NOT EXISTS (SELECT 1 FROM fragmented_meta WHERE friend_id=? AND frags_id=?);");
  bind_Int_Int64_Int64_Int_Int64(stmtInsertFragmentedDataInbound, friend_number, id, sz, friend_number, id);
  execPrepared(stmtInsertFragmentedDataInbound);

  prepare(&stmtInsertFragmentedMetaInbound,
    "INSERT INTO fragmented_meta (outbound, friend_id, type, frags_id, timestamp_first, timestamp_last,"
                                " frags_done, frags_num)"
    " SELECT 0, ?, ?, ?, ?, ?, 0, ?"
    " WHERE NOT EXISTS (SELECT 1 FROM fragmented_meta WHERE friend_id=? AND frags_id=?);");
  bind_Int_Int_Int64_Int64_Int64_Int_Int_Int64(stmtInsertFragmentedMetaInbound, friend_number, type, id, tm, tm, numParts, friend_number, id);
  execPrepared(stmtInsertFragmentedMetaInbound);

  uint64_t rowid = getFragmentsDataRowid(friend_number, id);
  if (!rowid) {
    dbUnlock(lock);
    return; // record is ready, must be a late duplicate
  }
  // write blob
#if defined(USE_BLOB_CACHE)
  if (!blobCache || rowid != blobCacheRowid) {
    if (blobCache)
      blobCacheCloseBlob();
    blobCache = openBlob("fragmented_data", "message", rowid);
    LOG("opened a blob object for rowid=%"PRIi64"", rowid)
    blobCacheRowid = rowid;
  }
  sqlite3_blob *blob = blobCache;
#else
  sqlite3_blob *blob = openBlob("fragmented_data", "message", rowid);
#endif
  uint8_t firstByte = 0;
  readBlob(blob, &firstByte, 1, off);
  if (firstByte && firstByte != data[0])
    WARNING("mismatching byte in blob: expected 0x%02x found 0x%02x for friend=%u msg id=%"PRIu64" partNo=%u numParts=%u off=%u sz=%u\n",
      data[0], firstByte, friend_number, id, partNo, numParts, off, sz);
  if (firstByte) {
#if !defined(USE_BLOB_CACHE)
    closeBlob(blob);
#endif
    return; // duplicate fragment received
  }
  writeBlob(blob, data, length, off);
  LOG("wrote a blob portion for rowid=%"PRIi64": length=%u off=%u", rowid, (unsigned)length, off)
#if !defined(USE_BLOB_CACHE)
  closeBlob(blob);
#endif
  updateFragmentedMetaDone(tm, friend_number, id);
  // see if the message is ready
  prepare(&stmtSelectFragmentedInboundDone,
    "SELECT timestamp_first, timestamp_last, friend_id, message, length(message)"
    " FROM fragmented_meta JOIN fragmented_data USING (friend_id, frags_id)"
    " WHERE outbound=0 AND friend_id=? AND frags_id=? AND frags_done = frags_num;");
  bind_Int_Int64(stmtSelectFragmentedInboundDone, friend_number, id);
  if (execPreparedRowOrNot(stmtSelectFragmentedInboundDone)) {
#if defined(USE_BLOB_CACHE)
    // blob isn't needed any more
    blobCacheCloseBlob();
#endif
    // notify the caller that the message is complete
    // the message is ready, notify the caller
    LOG("dbInsertInboundFragment >>> msgReadyCb")
    msgReadyCb(
      tox_opaque,
      sqlite3_column_int64(stmtSelectFragmentedInboundDone, 0),
      sqlite3_column_int64(stmtSelectFragmentedInboundDone, 1),
      sqlite3_column_int(stmtSelectFragmentedInboundDone, 2),
      type,
      (const uint8_t*)sqlite3_column_blob(stmtSelectFragmentedInboundDone, 3),
      sqlite3_column_int64(stmtSelectFragmentedInboundDone, 4),
      user_data);
    LOG("dbInsertInboundFragment <<< msgReadyCb")
    resetStmt(stmtSelectFragmentedInboundDone);
    LOG("dbInsertInboundFragment: done resetStmt")
    // delete the data record, only leave the meta record in order to ignore further duplicates
    deleteDataRecord(friend_number, id);
    LOG("dbInsertInboundFragment: done deleteDataRecord")
  } else {
    resetStmt(stmtSelectFragmentedInboundDone);
  }
  dbUnlock(lock);
}

FUNC_LOCAL void dbInsertOutboundMessage(uint32_t friend_number, int type, uint64_t id,
                                        uint64_t tm,
                                        unsigned numParts,
                                        const uint8_t *data, size_t length,
                                        uint32_t receipt) {
  void *lock = dbLock();
  prepare(&stmtInsertFragmentedMetaOutbound,
    "INSERT INTO fragmented_meta (outbound, friend_id, type, frags_id, timestamp_first, timestamp_last,"
                                " frags_done, frags_num)"
    " VALUES(1, ?, ?, ?, ?, ?, 0, ?);");
  bind_Int_Int_Int64_Int64_Int64_Int(stmtInsertFragmentedMetaOutbound, friend_number, type, id, tm, tm, numParts);
  execPrepared(stmtInsertFragmentedMetaOutbound);

  prepare(&stmtInsertFragmentedDataOutbound,
    "INSERT INTO fragmented_data (friend_id, frags_id, message, confirmed, receipt)"
    " VALUES(?, ?, ?, zeroblob(?), ?);");
  bind_Int_Int64_Blob_Int_Int(stmtInsertFragmentedDataOutbound, friend_number, id, data, length, numParts, receipt);
  execPrepared(stmtInsertFragmentedDataOutbound);
  dbUnlock(lock);
}

FUNC_LOCAL void dbOutboundPartConfirmed(uint32_t friend_number, uint64_t id, unsigned partNo, uint64_t tm) {
  void *lock = dbLock();
  uint64_t rowid = getFragmentsDataRowid(friend_number, id);
  if (!rowid)
    abort();
  uint8_t one = 1;
  sqlite3_blob *confirmedBlob = openBlob("fragmented_data", "confirmed", rowid);
  writeBlob(confirmedBlob, &one, 1, partNo-1);
  closeBlob(confirmedBlob);
  updateFragmentedMetaDone(tm, friend_number, id);
  dbUnlock(lock);
}

FUNC_LOCAL void dbLoadPendingSentMessages(DbMsgPendingSentCb msgPendingSentCb) {
  void *lock = dbLock();
  prepare(&stmtSelectFragmentedOutboundPending,
    "SELECT friend_id, type, frags_id,"
          " timestamp_first, timestamp_last,"
          " frags_done, frags_num,"
          " message, length(message),"
          " confirmed, length(confirmed),"
          " receipt"
    " FROM fragmented_meta JOIN fragmented_data USING (friend_id, frags_id)"
    " WHERE outbound=1;");
  while (execPreparedRowOrNot(stmtSelectFragmentedOutboundPending)) {
    msgPendingSentCb(
      sqlite3_column_int  (stmtSelectFragmentedOutboundPending, 0),
      sqlite3_column_int  (stmtSelectFragmentedOutboundPending, 1),
      sqlite3_column_int64(stmtSelectFragmentedOutboundPending, 2),
      sqlite3_column_int64(stmtSelectFragmentedOutboundPending, 3),
      sqlite3_column_int64(stmtSelectFragmentedOutboundPending, 4),
      sqlite3_column_int  (stmtSelectFragmentedOutboundPending, 5),
      sqlite3_column_int  (stmtSelectFragmentedOutboundPending, 6),
      (const uint8_t*)sqlite3_column_blob(stmtSelectFragmentedOutboundPending, 7),
      sqlite3_column_int  (stmtSelectFragmentedOutboundPending, 8),
      (const uint8_t*)sqlite3_column_blob(stmtSelectFragmentedOutboundPending, 9),
      sqlite3_column_int  (stmtSelectFragmentedOutboundPending, 10),
      sqlite3_column_int  (stmtSelectFragmentedOutboundPending, 11)
    );
  }
  resetStmt(stmtSelectFragmentedOutboundPending);
  dbUnlock(lock);
}

FUNC_LOCAL void dbClearPending(uint32_t friend_number, uint64_t id) {
  void *lock = dbLock();
  deleteDataRecord(friend_number, id);
  dbUnlock(lock);
}

FUNC_LOCAL void dbPeriodic() {
}

// internal definitions

static void* dbLock() {
  return dbLockCb ? dbLockCb(dbLockUserData) : NULL;
}

static void dbUnlock(void *lock) {
  if (lock)
    dbUnlockCb(lock, dbLockUserData);
}

static void initDb() {
  createSchema();
  readDbName(dbName);
}

static void execSql(const char *sql) {
  int rc;
  char *exec_errmsg;
  LOG("execSql: sql=%s", sql)
  if (CK_ERROR(sqlite3_exec(db, sql, NULL, NULL, &exec_errmsg)))
    errSql(rc, "executing sql", sql);
}

static void createSchema() {
  void *lock = dbLock();
  execSql(
    "CREATE TABLE IF NOT EXISTS fragmented_meta ("
    " outbound INTEGER NOT NULL,"
    " friend_id INTEGER NOT NULL,"
    " type INTEGER NOT NULL,"
    " frags_id INTEGER NOT NULL,"
    " timestamp_first INTEGER NOT NULL, timestamp_last INTEGER NOT NULL,"
    " frags_done INTEGER NOT NULL, frags_num INTEGER NOT NULL,"
    " PRIMARY KEY(friend_id, frags_id)); "
    "CREATE TABLE IF NOT EXISTS fragmented_data ("
    " friend_id INTEGER NOT NULL,"
    " frags_id INTEGER NOT NULL,"
    " message BLOB NULL,"
    " confirmed BLOB NULL,"
    " receipt INTEGER NULL,"
#if !defined(USE_SQLITE_WORKAROUND)
    " PRIMARY KEY(friend_id, frags_id));"
#else
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " UNIQUE(friend_id, frags_id));"
    "INSERT INTO sqlite_sequence VALUES('fragmented_data', 1000000000);"
#endif
  );
  dbUnlock(lock);
}

static void readDbName(char *name) {
  static sqlite3_stmt *stmt = NULL;
  prepare(&stmt, "PRAGMA database_list;");
  execPreparedText(stmt, 1, (unsigned char*)name);
  sqlite3_finalize(stmt);
}

static uint64_t getFragmentsDataRowid(uint32_t friend_number, uint64_t id) {
  prepare(&stmtSelectRowidFromFragmentedMeta,
    "SELECT rowid FROM fragmented_data WHERE friend_id=? AND frags_id=?;");
  bind_Int_Int64(stmtSelectRowidFromFragmentedMeta, friend_number, id);
  int64_t rowid = 0;
  if (execPreparedInt64(stmtSelectRowidFromFragmentedMeta, 0, &rowid))
    return rowid;
  else
    return 0;
}

static void updateFragmentedMetaDone(uint64_t tm, uint32_t friend_number, uint64_t id) {
  prepare(&stmtUpdateFragmentedMeta,
    "UPDATE fragmented_meta SET timestamp_last=max(timestamp_last,?), frags_done = frags_done+1"
    " WHERE friend_id=? AND frags_id=?;");
  bind_Int64_Int_Int64(stmtUpdateFragmentedMeta, tm, friend_number, id);
  execPrepared(stmtUpdateFragmentedMeta);
}

static void deleteDataRecord(uint32_t friend_number, uint64_t id) {
  LOG("deleteDataRecord: friend_number=%u id=%"PRIu64"", friend_number, id)
  prepare(&stmtDeleteFragmentedData,
    "DELETE FROM fragmented_data WHERE friend_id=? AND frags_id=?;");
  bind_Int_Int64(stmtDeleteFragmentedData, friend_number, id);
  execPrepared(stmtDeleteFragmentedData);
}

static sqlite3_stmt* prepareStatement(const char *sql) {
  int rc;
  sqlite3_stmt *stmt = NULL;
  const char *tail = NULL;
  if (CK_ERROR(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, &tail)))
    errSql(rc, "preparing statement", sql);
  return stmt;
}

static void destroyPreparedStatement(sqlite3_stmt **stmt) {
  if (*stmt) {
    sqlite3_finalize(*stmt);
    *stmt = NULL;
  }
}

static void destroyPreparedStatements() {
  destroyPreparedStatement(&stmtInsertFragmentedDataInbound);
  destroyPreparedStatement(&stmtInsertFragmentedMetaInbound);
  destroyPreparedStatement(&stmtSelectRowidFromFragmentedMeta);
  destroyPreparedStatement(&stmtInsertFragmentedDataOutbound);
  destroyPreparedStatement(&stmtInsertFragmentedMetaOutbound);
  destroyPreparedStatement(&stmtUpdateFragmentedMeta);
  destroyPreparedStatement(&stmtSelectFragmentedInboundDone);
  destroyPreparedStatement(&stmtSelectFragmentedOutboundPending);
  destroyPreparedStatement(&stmtDeleteFragmentedData);
}

static void prepare(sqlite3_stmt **pstmt, const char *sql) {
  if (*pstmt == NULL)
    *pstmt = prepareStatement(sql);
}

static void bindInt(sqlite3_stmt *stmt, int n, int a) {
  int rc;
  if (CK_ERROR(sqlite3_bind_int(stmt, n, a)))
    errSql(rc, "binding int value", sqlite3_sql(stmt));
}

static void bindInt64(sqlite3_stmt *stmt, int n, sqlite3_int64 a) {
  int rc;
  if (CK_ERROR(sqlite3_bind_int64(stmt, n, a)))
    errSql(rc, "binding int64 value", sqlite3_sql(stmt));
}

static void bindBlob(sqlite3_stmt *stmt, int n, const uint8_t *data, size_t size) {
  int rc;
  if (CK_ERROR(sqlite3_bind_blob64(stmt, n, data, size, NULL)))
    errSql(rc, "binding blob value", sqlite3_sql(stmt));
}


static void bind_Int_Int64(sqlite3_stmt *stmt, int a1, sqlite3_int64 a2) {
  bindInt  (stmt, 1, a1);
  bindInt64(stmt, 2, a2);
}

static void bind_Int64_Int_Int64(sqlite3_stmt *stmt, sqlite3_int64 a1, int a2, sqlite3_int64 a3) {
  bindInt64(stmt, 1, a1);
  bindInt  (stmt, 2, a2);
  bindInt64(stmt, 3, a3);
}

static void bind_Int_Int64_Blob_Int_Int(sqlite3_stmt *stmt,
                                        int a1, sqlite3_int64 a2, const uint8_t *a3data, size_t a3size,
                                        int a4, int a5) {
  bindInt  (stmt, 1, a1);
  bindInt64(stmt, 2, a2);
  bindBlob (stmt, 3, a3data, a3size);
  bindInt  (stmt, 4, a4);
  bindInt  (stmt, 5, a5);
}

static void bind_Int_Int64_Int64_Int_Int64(sqlite3_stmt *stmt,
                                           int a1, sqlite3_int64 a2, sqlite3_int64 a3, int a4,
                                           sqlite3_int64 a5) {
  bindInt  (stmt, 1, a1);
  bindInt64(stmt, 2, a2);
  bindInt64(stmt, 3, a3);
  bindInt64(stmt, 4, a4);
  bindInt64(stmt, 5, a5);
}

static void bind_Int_Int_Int64_Int64_Int64_Int(sqlite3_stmt *stmt,
                                               int a1, int a2, sqlite3_int64 a3, sqlite3_int64 a4,
                                               sqlite3_int64 a5, int a6) {
  bindInt  (stmt, 1, a1);
  bindInt  (stmt, 2, a2);
  bindInt64(stmt, 3, a3);
  bindInt64(stmt, 4, a4);
  bindInt64(stmt, 5, a5);
  bindInt  (stmt, 6, a6);
}

static void bind_Int_Int_Int64_Int64_Int64_Int_Int_Int64(sqlite3_stmt *stmt,
                                                         int a1, int a2, sqlite3_int64 a3, sqlite3_int64 a4,
                                                         sqlite3_int64 a5, int a6, int a7, sqlite3_int64 a8) {
  bindInt  (stmt, 1, a1);
  bindInt  (stmt, 2, a2);
  bindInt64(stmt, 3, a3);
  bindInt64(stmt, 4, a4);
  bindInt64(stmt, 5, a5);
  bindInt  (stmt, 6, a6);
  bindInt  (stmt, 7, a7);
  bindInt64(stmt, 8, a8);
}

static void execPrepared(sqlite3_stmt *stmt) {
  int rc;
  if ((rc = sqlite3_step(stmt)) != SQLITE_DONE)
    errSql(rc, "executing prepared statement", sqlite3_sql(stmt));
  resetStmt(stmt);
  LOG("execPrepared: sql=%s", sqlite3_sql(stmt))
}

static int execPreparedRowOrNot(sqlite3_stmt *stmt) {
  int rc;
  if ((rc = sqlite3_step(stmt)) != SQLITE_ROW && rc != SQLITE_DONE)
    errSql(rc, "executing prepared statement", sqlite3_sql(stmt));
  LOG("execPreparedRowOrNot: sql=%s rowExists=%s", sqlite3_sql(stmt), rc == SQLITE_ROW ? "YES" : "NO")
  return rc == SQLITE_ROW;
}

static int execPreparedInt64(sqlite3_stmt *stmt, int iCol, int64_t *value) {
  int rc;
  if ((rc = sqlite3_step(stmt)) != SQLITE_ROW && rc != SQLITE_DONE)
    errSql(rc, "executing prepared statement", sqlite3_sql(stmt));
  LOG("execPreparedInt64: sql=%s rowExists=%s ...", sqlite3_sql(stmt), rc == SQLITE_ROW ? "YES" : "NO")
  if (rc == SQLITE_ROW) {
    LOG("execPreparedInt64: ... rowExists=%s -> %lli", rc == SQLITE_ROW ? "YES" : "NO", sqlite3_column_int64(stmt, iCol))
    *value = sqlite3_column_int64(stmt, iCol);
    resetStmt(stmt);
    return 1;
  } else {
    resetStmt(stmt);
    return 0;
  }
}

static int execPreparedText(sqlite3_stmt *stmt, int iCol, unsigned char *value) {
  int rc;
  if ((rc = sqlite3_step(stmt)) != SQLITE_ROW && rc != SQLITE_DONE)
    errSql(rc, "executing prepared statement", sqlite3_sql(stmt));
  LOG("execPreparedText: sql=%s rowExists=%s ...", sqlite3_sql(stmt), rc == SQLITE_ROW ? "YES" : "NO")
  if (rc == SQLITE_ROW) {
    LOG("execPreparedText: ... rowExists=%s -> %s", rc == SQLITE_ROW ? "YES" : "NO", (const char*)sqlite3_column_text(stmt, iCol))
    strcpy((char*)value, (const char*)sqlite3_column_text(stmt, iCol));
    resetStmt(stmt);
    return 1;
  } else {
    resetStmt(stmt);
    return 0;
  }
}

static void resetStmt(sqlite3_stmt *stmt) {
  sqlite3_reset(stmt);
}

static void err(int rc, const char *op) {
  fprintf(stderr, "Error while %s: error=%s (%d)\n", op, sqlite3_errstr(rc), rc);
  abort();
}

static void errSql(int rc, const char *op, const char *sql) {
  fprintf(stderr, "Error while %s: sql=%s error=%s (%d)\n", op, sql, sqlite3_errstr(rc), rc);
  abort();
}

static sqlite3_blob* openBlob(const char *table, const char *field, uint64_t rowid) {
  int rc;
  sqlite3_blob *blob;
  if (CK_ERROR(sqlite3_blob_open(db, dbName,
                                 table,
                                 field,
                                 rowid,
                                 1, /*flags: writable*/
                                 &blob)))
    err(rc, "opening blob");
  return blob;
}

static void readBlob(sqlite3_blob *blob, uint8_t *data, unsigned length, unsigned off) {
  int rc;
  if (CK_ERROR(sqlite3_blob_read(blob, data, length, off)))
    err(rc, "reading blob");
}

static void writeBlob(sqlite3_blob *blob, const uint8_t *data, unsigned length, unsigned off) {
  int rc;
  if (CK_ERROR(sqlite3_blob_write(blob, data, length, off)))
    err(rc, "writing blob");
}

static void closeBlob(sqlite3_blob *blob) {
  int rc;
  if (CK_ERROR(sqlite3_blob_close(blob)))
    err(rc, "closing blob");
}

#if defined(USE_BLOB_CACHE)
static void blobCacheCloseBlob() {
  closeBlob(blobCache);
  blobCache = NULL;
  blobCacheRowid = 0;
}
#endif


//
// Copyright © 2017 by Yuri Victorovich. All rights reserved.
//

#include <stdint.h>
#include <stddef.h>

// callbacks
typedef void* (*DbLockCb)(void *user_data);
typedef void (*DbUnlockCb)(void*, void *user_data);
typedef void (*DbMsgReadyCb)(void *tox_opaque, uint64_t tm1, uint64_t tm2, uint32_t friend_number, int type, const uint8_t *message, size_t length, void *user_data);
typedef void (*DbMsgPendingSentCb)(uint32_t friend_number, int type, uint64_t id,
                                   uint64_t tm1,
                                   uint64_t tm2,
                                   unsigned numConfirmed,
                                   unsigned numParts,
                                   const uint8_t *message,
                                   unsigned lengthMessage,
                                   const uint8_t *confirmed,
                                   unsigned lengthConfirmed,
                                   int receipt);

// interface
void dbInitialize(sqlite3 *new_db, DbLockCb lockCb, DbUnlockCb unlockCb, void *user_data);
void dbInitializeInMemory();
void dbUninitialize();
void dbInsertInboundFragment(void *tox_opaque,
                             uint32_t friend_number, int type, uint64_t id,
                             unsigned partNo, unsigned numParts, unsigned off, unsigned sz,
                             const uint8_t *data, size_t length,
                             uint64_t tm,
                             DbMsgReadyCb msgReadyCb,
                             void *user_data);
void dbInsertOutboundMessage(uint32_t friend_number, int type, uint64_t id,
                             uint64_t tm,
                             unsigned numParts,
                             const uint8_t *data, size_t length,
                             uint32_t receipt);
void dbOutboundPartConfirmed(uint32_t friend_number, uint64_t id, unsigned partNo, uint64_t tm);
void dbLoadPendingSentMessages(DbMsgPendingSentCb msgPendingSentCb);
void dbClearOutboundPending(uint32_t friend_number, uint64_t id);
void dbPeriodic();

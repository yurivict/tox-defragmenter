#ifndef TOX_DEFRAGMENTER_H
#define TOX_DEFRAGMENTER_H

//
// Copyright © 2017 by Yuri Victorovich. All rights reserved.
//

#include <tox/tox.h>
typedef struct sqlite3 sqlite3;

#ifdef __cplusplus
extern "C" {
#endif

typedef void* (*ToxDefragmenterDbLockCb)(void *user_data);
typedef void (*ToxDefragmenterDbUnlockCb)(void*, void *user_data);

ToxcoreApi tox_defragmenter_initialize_api(const ToxcoreApi *api);
void tox_defragmenter_initialize_db(sqlite3* db, ToxDefragmenterDbLockCb lockCb, ToxDefragmenterDbUnlockCb unlockCb, void *user_data);
void tox_defragmenter_uninitialize();
void tox_defragmenter_periodic(Tox *tox);

#ifdef __cplusplus
}
#endif

#endif //TOX_DEFRAGMENTER_H

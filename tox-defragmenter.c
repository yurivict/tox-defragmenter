//
// Copyright © 2017 by Yuri Victorovich. All rights reserved.
//

#include "common.h"
#include "tox-defragmenter.h"
#include "database.h"
#include "marker.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#define LOG(op, fmt...) //utilLog(__FUNCTION__, "Main." op, fmt);

static Tox *toxInstance = NULL;
static uint8_t initializedApi = 0;
static uint8_t initializedDb = 0;
static ToxcoreApi base_toxcore_api;
#define TOX(function) base_toxcore_api.tox_##function
#define MY(function) tox_defragmenter_##function
#define CLIENT(function) client_##function
static tox_friend_read_receipt_cb *client_friend_read_receipt_cb = 0;
static tox_friend_message_cb *client_friend_message_cb = 0;
static unsigned markerMaxSizeEver = 0;
static uint64_t lastMsgId = 0;

#define NEW(type) ((type*)calloc(sizeof(type), 1))
#define NEWA(elt, num) ((elt*)calloc(sizeof(elt), num))
#define REALLOC(old, elt, numOld, numNew) ((elt*)memRealloc(old, sizeof(elt)*numOld, sizeof(elt)*numNew))
#define DEL(obj) free(obj)
#define MVA(arr, idx1, idx2, idxDelta) { \
  memmove(arr+idx1+idxDelta, arr+idx1, sizeof(arr[0])*(idx2-idx1)); \
  memset(idxDelta > 0 ? arr+idx1 : arr+idx2-idxDelta, 0, sizeof(arr[0])*idxDelta); \
}

struct params {
  unsigned maxMessageLength;
  unsigned fragmentsAtATime;
  unsigned receiptExpirationTimeMs;
  uint32_t receiptRangeLo;
  uint32_t receiptRangeHi;
} params = {
  // defaults
  TOX_MAX_MESSAGE_LENGTH,
  512,        // 512 packets at a time
  20000,      // 20 sec
  0x70000000, // receipt range low
  0x7fffffff  // receipt range high
};

#define FID "%"PRIu64
#define FTM "%"PRIu64

//
// structures
//

typedef struct fragment {
  size_t          length;
  uint8_t         *data;
  uint32_t        receipt;   // receipt from below that we are waiting for
  unsigned        timesSent; // how many times did we send it
  int             confirmed; // receipt received
} fragment;

typedef struct msg_outbound {
  struct msg_outbound *prev;
  struct msg_outbound *next;
  uint32_t         friend_number;
  uint64_t         id;
  TOX_MESSAGE_TYPE type;
  unsigned         numParts;
  fragment         *fragments;
  uint32_t         receipt;     // receipt number we sent to the client
  unsigned         lastSent;
  unsigned         numTransit;
  unsigned         numConfirmed;
  unsigned         numLoss;
  int              fromDb;
} msg_outbound;

typedef struct receipt_record {
  uint32_t      receipt;
  msg_outbound  *msg;
  unsigned      partNo;
  uint64_t      timestamp;
} receipt_record;

//
// static data
//
static msg_outbound *msgsOutbound = NULL;
static receipt_record *receipts = 0;
static unsigned receiptsLo = 0;
static unsigned receiptsHi = 0;
static unsigned receiptsNum = 0;
static unsigned receiptsAlloc = 0;
static uint32_t lastReceipt = 0;

//
// declarations
//
static void* memRealloc(void *mem, unsigned szOld, unsigned szNew);
static uint64_t getCurrTimeMs();
static uint32_t generateReceiptNo();
static void initialize();
static void uninitialize();
static void receiptsInitialize();
static void receiptsUninitialize();
static void MY(callback_friend_read_receipt)(Tox *tox, tox_friend_read_receipt_cb *callback);
static void MY(callback_friend_message)(Tox *tox, tox_friend_message_cb *callback);
static void msgsOutboundLink(msg_outbound *msg);
static void msgsOutboundUnlink(msg_outbound *msg);
static void msgOutboundDelete(msg_outbound *msg);
static void msgsOutboundDeleteAll();
static int isFriendOnline(Tox *tox, uint32_t friend_number);
static Tox* MY(new)(const struct Tox_Options *options, TOX_ERR_NEW *error);
static void MY(kill)(Tox *tox);
static uint32_t MY(friend_send_message)(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                        size_t length, TOX_ERR_FRIEND_SEND_MESSAGE *error);
static uint32_t MY(friend_send_message_long)(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                             size_t length, TOX_ERR_FRIEND_SEND_MESSAGE *error);
static void msgSendNextParts(Tox *tox, msg_outbound *msg);
static void msgIsComplete(Tox *tox, msg_outbound *msg, void *user_data);
static int msgSendPart(Tox *tox, msg_outbound *msg, unsigned i);
static msg_outbound* splitMessage(const uint8_t *message, size_t length, size_t maxLength, uint64_t id);
static void addReceipt(uint32_t receipt, msg_outbound *msg, unsigned partNo, uint64_t timestamp);
static int findReceipt(uint32_t receipt);
static void compressReceipts();
static void resendExpiredReceipts(Tox *tox);
static void sendMore(Tox *tox);
static void loadPendingSentMessages();
static void loadPendingSentMessage(uint32_t friend_number, int type, uint64_t id,
                                   uint64_t tm1,
                                   uint64_t tm2,
                                   unsigned numConfirmed,
                                   unsigned numParts,
                                   const uint8_t *message,
                                   unsigned lengthMessage,
                                   const uint8_t *confirmed,
                                   unsigned lengthConfirmed,
                                   int receipt);
static void MY(friend_read_receipt_cb)(Tox *tox, uint32_t friend_number, uint32_t message_id, void *user_data);
static int tryProcessReceipt(Tox *tox, uint32_t receipt, void *user_data);
static int isFragment(const uint8_t *message, size_t length);
static void MY(friend_message_cb)(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                  size_t length, void *user_data);
static void messageReady(void *tox_opaque,
                         uint64_t tm1, uint64_t tm2,
                         uint32_t friend_number, int type, const uint8_t *message, size_t length, void *user_data);
static void processInFragment(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                              size_t length, void *user_data);
static void doPeriodic(Tox *tox);

//
// utils
//

static void* memRealloc(void *mem, unsigned szOld, unsigned szNew) {
  mem = realloc(mem, szNew);
  if (szNew > szOld)
    memset(mem + szOld, 0, szNew - szOld);
  return mem;
}

static uint64_t getCurrTimeMs() {
  struct timeval tm;
  gettimeofday(&tm, NULL);
  return tm.tv_sec*1000 + tm.tv_usec/1000;
}

static uint64_t generateMsgId() {
  uint64_t msgId = getCurrTimeMs();
  // prevent id collisions in case of a very fast message creation
  while (msgId <= lastMsgId)
    msgId++;
  lastMsgId = msgId;
  return msgId;
}

static uint32_t generateReceiptNo() {
  lastReceipt = lastReceipt+1 <= params.receiptRangeHi ? lastReceipt+1 : params.receiptRangeLo;
  // avoid possible conflict with receipts of the pending packets loaded from db
  if (msgsOutbound) {
    int changed;
    do {
      changed = 0;
      msg_outbound *m = msgsOutbound;
      do {
        if (m->receipt == lastReceipt) {
          lastReceipt = lastReceipt+1 <= params.receiptRangeHi ? lastReceipt+1 : params.receiptRangeLo;
          changed = 1;
          break;
        }
        m = m->next;
      } while (m != msgsOutbound);
    } while (changed);
  }
  return lastReceipt;
}

static void initialize() {
  receiptsInitialize();
  loadPendingSentMessages();
  markerMaxSizeEver = markerMaxSizeBytes(INT_MAX, INT_MAX);
}

static void uninitialize() {
  msgsOutboundDeleteAll();
  receiptsUninitialize();
}

static void receiptsInitialize() {
  receiptsLo = 0;
  receiptsHi = 0;
  receiptsNum = 0;
  receiptsAlloc = 32;
  receipts = NEWA(receipt_record, receiptsAlloc);
  lastReceipt = params.receiptRangeLo;
}

static void receiptsUninitialize() {
  DEL(receipts);
}

//
// callbacks
//

static void MY(callback_friend_read_receipt)(Tox *tox, tox_friend_read_receipt_cb *callback) {
  CLIENT(friend_read_receipt_cb) = callback;
  TOX(callback_friend_read_receipt)(tox, MY(friend_read_receipt_cb));
}

static void MY(callback_friend_message)(Tox *tox, tox_friend_message_cb *callback) {
  CLIENT(friend_message_cb) = callback;
  TOX(callback_friend_message)(tox, MY(friend_message_cb));
}

//
// thread
//

static pthread_t thread;
static int threadStopFlag = 0;

static void* threadRoutine(void *arg) {
  while (!threadStopFlag) {
    sleep(2);
    if (toxInstance)
      doPeriodic(toxInstance);
  }
  return NULL;
}

static __attribute__((constructor)) void threadInit(void) {
  LOG("starting thread")
  int res;
  if ((res = pthread_create(&thread, 0/*attr*/, &threadRoutine, 0/*arg*/)))
    ERROR("Failed to start the thread, error=%d", res)
}

static __attribute__((destructor)) void threadFini(void) {
  LOG("stopping thread")
  void *v;
  threadStopFlag = 1;
  int res;
  if ((res = pthread_join(thread, &v)))
    ERROR("Failed to stop the thread, error=%d", res)
}

//
// SEND
//

static void msgsOutboundLink(msg_outbound *msg) {
  if (msgsOutbound) {
    msg->next = msgsOutbound->next;
    msg->prev = msgsOutbound;
    msgsOutbound->next->prev = msg;
    msgsOutbound->next = msg;
  } else {
    msgsOutbound = msg->prev = msg->next = msg;
  }
}

static void msgsOutboundUnlink(msg_outbound *msg) {
  if (msg == msgsOutbound) {
    if (msg->next != msg->prev)
      msgsOutbound = msg->prev;
    else
      msgsOutbound = NULL;
  }
  msg->next->prev = msg->prev;
  msg->prev->next = msg->next;
}

static void msgOutboundDelete(msg_outbound *msg) {
  // free
  for (int i = 0; i < msg->numParts; i++)
    free(msg->fragments[i].data);
  free(msg->fragments);
  free(msg);
}

static void msgsOutboundDeleteAll() {
  while (msgsOutbound) {
    msg_outbound *msg = msgsOutbound;
    msgsOutboundUnlink(msg);
    msgOutboundDelete(msg);
  }
}

static int isFriendOnline(Tox *tox, uint32_t friend_number) {
  return TOX(friend_get_connection_status)(tox, friend_number, NULL) != TOX_CONNECTION_NONE;
}

static Tox* MY(new)(const struct Tox_Options *options, TOX_ERR_NEW *error) {
  if (toxInstance)
    ERROR("Multiple Tox instances aren't yet suported.")
  toxInstance = MY(new)(options, error);
  return toxInstance;
}

static void MY(kill)(Tox *tox) {
  if (toxInstance != tox)
    ERROR("Tox instance mismatch.")
  MY(kill)(tox);
  toxInstance = NULL;
}

static uint32_t MY(friend_send_message)(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                        size_t length, TOX_ERR_FRIEND_SEND_MESSAGE *error) {
  // prevent the client from sending the fragment signature
  if (markerExists(message, length))
    return 0;
  // send
  if (length <= params.maxMessageLength) {
    LOG("SEND", "passing through the short outgoing message of length=%d for friend_number=%d",
      (unsigned)length, friend_number)
    return TOX(friend_send_message)(tox, friend_number, type, message, length, error);
  } else {
    LOG("SEND", "GOT LONG MESSAGE with length=%d for friend_number=%d, splitting ...",
      (unsigned)length, friend_number)
    return MY(friend_send_message_long)(tox, friend_number, type, message, length, error);
  }
}

static uint32_t MY(friend_send_message_long)(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                             size_t length, TOX_ERR_FRIEND_SEND_MESSAGE *error) {
  msg_outbound *msg = splitMessage(message, length, params.maxMessageLength, generateMsgId());
  msg->friend_number = friend_number;
  for (unsigned i = 0; i < msg->numParts; i++) {
    if (msg->numTransit < params.fragmentsAtATime) {
      if (msgSendPart(tox, msg, i))
        msg->lastSent = i;
    } else {
      break;
    }
  }
  if (msg->numTransit > 0) {
    // fill the remaining fields
    msg->type = type;
    msg->receipt = generateReceiptNo();
    // insert into the list
    msgsOutboundLink(msg);
    // add to db
    dbInsertOutboundMessage(friend_number, type, msg->id, msg->id, msg->numParts,
                            message, length,
                            msg->receipt);
    // return the receipt
    LOG("SEND", "returning receipt # to client: msg=%p length=%u msg.numParts=%u, sending receipt %x to the client",
      msg, (unsigned)length, msg->numParts, msg->receipt)
    return msg->receipt;
  } else {
    // failure to send all attempted parts translates into inability to send the whole message
    LOG("SEND", "failed to send the message of length=%u msg.numParts=%u, returning receipt 0 to the client",
      (unsigned)length, msg->numParts)
    msgOutboundDelete(msg);
    return 0;
  }
}

static void msgSendNextParts(Tox *tox, msg_outbound *msg) {
  // original pass through the fragments
  for (unsigned i = msg->lastSent+1; i < msg->numParts; i++) {
    if (msg->numTransit < params.fragmentsAtATime) {
      if (msgSendPart(tox, msg, i))
        msg->lastSent = i;
    } else {
      break;
    }
  }
  // send fragments that failed before for some reason
  for (unsigned i = 0; i < msg->numParts; i++) {
    if (msg->numTransit < params.fragmentsAtATime &&
        msg->numTransit + msg->numConfirmed < msg->numParts) {
      if (!msg->fragments[i].receipt &&
          !msg->fragments[i].confirmed)
        msgSendPart(tox, msg, i);
    } else {
      break;
    }
  }
}

static void msgIsComplete(Tox *tox, msg_outbound *msg, void *user_data) {
  LOG("SEND", "msg=%p id="FID" msg.numParts=%u, sending receipt %x to the client",
    msg, msg->id, msg->numParts, msg->receipt)
  CLIENT(friend_read_receipt_cb)(tox, msg->friend_number, msg->receipt, user_data);
  dbClearOutboundPending(msg->friend_number, msg->id);
  msgsOutboundUnlink(msg);
  msgOutboundDelete(msg);
}

static int msgSendPart(Tox *tox, msg_outbound *msg, unsigned i) {
  uint32_t receipt = TOX(friend_send_message)(tox, msg->friend_number, msg->type,
                                              msg->fragments[i].data, msg->fragments[i].length, NULL);
  if (!receipt)
    return 0;
  msg->fragments[i].receipt = receipt;
  msg->fragments[i].timesSent++;
  addReceipt(receipt, msg, i+1, getCurrTimeMs());
  msg->numTransit++;
  LOG("SEND", "sent partNo=%u of msg=%p id="FID
             " length=%u of msg=%p part.timesSent=%u msg.numTransit=%u msg.numConfirmed=%u msg.numParts=%u",
    i, msg, msg->id,
    (unsigned)msg->fragments[i].length, msg, msg->fragments[i].timesSent, msg->numTransit, msg->numConfirmed, msg->numParts)
  return 1;
}

static msg_outbound* splitMessage(const uint8_t *message, size_t length, size_t maxLength, uint64_t id) {
  uint8_t maxMarker = markerMaxSizeBytes((length + maxLength-markerMaxSizeEver - 1)/(maxLength-markerMaxSizeEver), length); // conservative estimate
  maxLength -= maxMarker;
  const uint8_t *m = message;
  unsigned numParts = (length + maxLength - 1)/maxLength;
  fragment *fragments = NEWA(fragment, numParts);
  fragment *f = fragments;

  unsigned off = 0;
  unsigned len = length;
  for (unsigned partNo = 1; len > 0; partNo++) {
    size_t step = len >= maxLength ? maxLength : len;
    uint8_t marker[maxMarker+1];
    uint8_t markerSize = markerPrint(id, partNo, numParts, off, length, marker);
    *f = (fragment){.length = markerSize+step, .data = NEWA(uint8_t, markerSize+step), .receipt = 0};
    memcpy(f->data, marker, markerSize);
    memcpy(f->data+markerSize, m, step);
    //
    len -= step;
    m   += step;
    off += step;
    f++;
  }
  msg_outbound *msg = NEW(msg_outbound);
  *msg = (msg_outbound){.id = id, .numParts = numParts, .fragments = fragments};
  return msg;
}

static void addReceipt(uint32_t receipt, msg_outbound *msg, unsigned partNo, uint64_t timestamp) {
  if (receiptsHi == 0 || receipts[receiptsHi-1].receipt < receipt) {
    if (receiptsHi == receiptsAlloc) {
      receipts = REALLOC(receipts, receipt_record, receiptsAlloc, 2*receiptsAlloc);
      receiptsAlloc *= 2;
    }
    receipts[receiptsHi] = (receipt_record){.receipt = receipt, .msg = msg, .partNo = partNo, .timestamp = timestamp};
    receiptsHi++;
  } else {
    int recIdx = findReceipt(receipt) + 1;
    if (receipts[recIdx].receipt) {
      while (receiptsHi + 16 > receiptsAlloc) {
        receipts = REALLOC(receipts, receipt_record, receiptsAlloc, 2*receiptsAlloc);
        receiptsAlloc *= 2;
      }
      MVA(receipts, recIdx, receiptsHi, +16)
      receipts[recIdx] = (receipt_record){.receipt = receipt, .msg = msg, .partNo = partNo, .timestamp = timestamp};
    } else {
      receipts[recIdx] = (receipt_record){.receipt = receipt, .msg = msg, .partNo = partNo, .timestamp = timestamp};
    }
  }
  receiptsNum++;
}

static int findReceipt(uint32_t receipt) {
  if (receiptsLo == receiptsHi)
    return -1;
  int i1 = receiptsLo;
  int i2 = receiptsHi;
  while (i1+1 < i2) {
    int im = (i1 + i2)/2;
    int imu = im;
    while (imu+1 < i2 && !receipts[imu].receipt)
      imu++;
    if (imu+1 < i2)
      if (receipts[imu].receipt < receipt)
        i1 = imu;
      else if (receipt < receipts[imu].receipt)
        i2 = im;
      else
        return imu;
    else
      i2 = im;
  }
  return receipts[i1].receipt < receipt ? i2 : i1;
}

static void compressReceipts() {
  receipt_record *r = NEWA(receipt_record, receiptsNum*2);
  receipt_record *r1 = r;
  for (int i = receiptsLo; i < receiptsHi; i++) {
    if (receipts[i].receipt)
      *r1++ = receipts[i];
  }
  free(receipts);
  receipts = r;
  receiptsLo = 0;
  receiptsHi = receiptsNum;
  receiptsAlloc = receiptsNum*2;
}

static void resendExpiredReceipts(Tox *tox) {
  if (!receiptsNum)
    return;
  uint64_t now = getCurrTimeMs();
  for (int i = receiptsLo; i < receiptsHi; i++)
    if (receipts[i].receipt && receipts[i].timestamp + params.receiptExpirationTimeMs < now) {
      // clear receipt
      receipts[i].receipt = 0;
      receiptsNum--;
      // clear transit count
      receipts[i].msg->numTransit--;
      receipts[i].msg->numLoss++;
      // resend
      msgSendPart(tox, receipts[i].msg, receipts[i].partNo-1);
    }
}

static void sendMore(Tox *tox) {
  if (!msgsOutbound)
    return;
  msg_outbound *msg = msgsOutbound;
  do {
    LOG("SEND", "trying to send more of the message msg=%p id="FID
               " numParts=%u numTransit=%u numConfirmed=%u",
      msg, msg->id, msg->numParts, msg->numTransit, msg->numConfirmed)
    if (isFriendOnline(tox, msg->friend_number)) {
      msgSendNextParts(tox, msg);
    } else {
      LOG("SEND", "skipping msg=%p id="FID
                 " numParts=%u numConfirmed=%u for friend=%u because this friend isn't onlinen",
        msg, msg->id, msg->numParts, msg->numConfirmed, msg->friend_number)
    }
    msg = msg->next;
  } while (msg != msgsOutbound);
}

static void loadPendingSentMessages() {
  dbLoadPendingSentMessages(&loadPendingSentMessage);
}

static void loadPendingSentMessage(uint32_t friend_number, int type, uint64_t id,
                                   uint64_t tm1,
                                   uint64_t tm2,
                                   unsigned numConfirmed,
                                   unsigned numParts,
                                   const uint8_t *message,
                                   unsigned lengthMessage,
                                   const uint8_t *confirmed,
                                   unsigned lengthConfirmed, int receipt) {
  LOG("SEND", "friend=%u type=%d id="FID" length=%u numConfirmed=%u numParts=%u",
    friend_number, type, id, lengthMessage, numConfirmed, numParts)
  msg_outbound *msg = splitMessage(message, lengthMessage, params.maxMessageLength, id);
  if (msg->numParts != numParts || msg->numParts != lengthConfirmed) {
    WARNING("mismatching number of parts of the pending outbound message for friend=%d msg=%p id="FID
            ": expected %u, got %u parts and %u confirmations, discarding the message\n",
      friend_number, msg, id, msg->numParts, numParts, lengthConfirmed)
    dbClearOutboundPending(friend_number, id);
    msgOutboundDelete(msg);
    return;
  }
  for (unsigned i = 0; i < msg->numParts; i++) {
    msg->fragments[i].confirmed = confirmed[i];
    if (confirmed[i]) {
      free(msg->fragments[i].data);
      msg->fragments[i].data = NULL;
      msg->numConfirmed++;
    }
  }
  if (numConfirmed != msg->numConfirmed || numConfirmed > numParts) {
    WARNING("mismatched or invalid confirmed count for friend=%d msg=%p id="FID": %u vs. %u, discarding the message\n",
      friend_number, msg, id, numConfirmed, msg->numConfirmed)
    dbClearOutboundPending(friend_number, id);
    msgOutboundDelete(msg);
    return;
  }
  if (msg->numConfirmed < numParts) {
    msg->friend_number = friend_number;
    msg->fromDb = 1;
    msgsOutboundLink(msg);
  } else {
    WARNING("all %u message parts are confirmed for friend=%u msg=%p id="FID", discarding the message\n",
      numParts, friend_number, msg, id)
    dbClearOutboundPending(friend_number, id);
    msgOutboundDelete(msg);
  }
}

// receipts

static void MY(friend_read_receipt_cb)(Tox *tox, uint32_t friend_number, uint32_t message_id, void *user_data) {
  LOG("SEND", "GOT receipt: friend_number=%d message_id=%u user_data=%p",
    friend_number, message_id, user_data)
  if (!tryProcessReceipt(tox, message_id, user_data))
    CLIENT(friend_read_receipt_cb)(tox, friend_number, message_id, user_data);
}

static int tryProcessReceipt(Tox *tox, uint32_t receipt, void *user_data) {
  int recIdx = findReceipt(receipt);
  if (recIdx == -1 || receipts[recIdx].receipt != receipt)
    return (params.receiptRangeLo <= receipt && receipt <= params.receiptRangeHi); // in range -> must be a duplicate receipt
  receipt_record *r = &receipts[recIdx];
  msg_outbound *msg = r->msg;
  fragment *f = &msg->fragments[r->partNo-1];
  f->receipt = 0;
  f->confirmed = 1;
  msg->numConfirmed++;
  msg->numTransit--;
  free(f->data);
  f->data = NULL;
  dbOutboundPartConfirmed(msg->friend_number, msg->id, r->partNo, getCurrTimeMs());
  LOG("SEND", "found receipt=%u: msg=%p id="FID" for friend_number=%d"
             " partNo=%u timeout="FTM" msg.numTransit=%u msg.numConfirmed=%u msg.numParts=%u",
      receipt, msg, msg->id, msg->friend_number,
      r->partNo, r->timestamp, msg->numTransit, msg->numConfirmed, msg->numParts)
  if (msg->numConfirmed < msg->numParts)
    if (isFriendOnline(tox, msg->friend_number)) {
      msgSendNextParts(tox, msg);
    } else {
      LOG("SEND", "skipping msg=%p id="FID
                 " numParts=%u for friend=%u because this friend isn't online",
        msg, msg->id, msg->numParts, msg->friend_number)
    }
  else
    msgIsComplete(tox, msg, user_data);
  // clear the receipt
  receipts[recIdx].receipt = 0;
  if (recIdx == receiptsLo) {
    do {
      receiptsLo++;
    } while (receiptsLo < receiptsHi && !receipts[receiptsLo].receipt);
  } else if (recIdx+1 == receiptsHi) {
    do {
      receiptsHi--;
    } while (receiptsLo < receiptsHi && !receipts[receiptsHi-1].receipt);
  }
  if (receiptsLo == receiptsHi)
    receiptsLo = receiptsHi = 0;
  return 1;
}

//
// RECV
//

static int isFragment(const uint8_t *message, size_t length) {
  return markerExists(message, length);
}

static void MY(friend_message_cb)(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                  size_t length, void *user_data) {
  if (isFragment(message, length))
    processInFragment(tox, friend_number, type, message, length, user_data);
  else {
    LOG("RECV", "passing through the incoming message length=%d", (unsigned)length)
    CLIENT(friend_message_cb)(tox, friend_number, type, message, length, user_data);
  }
}

static void messageReady(void *tox_opaque,
                         uint64_t tm1, uint64_t tm2,
                         uint32_t friend_number, int type, const uint8_t *message, size_t length, void *user_data) {
  LOG("RECV", "forwarding the message of length=%u to the client", (unsigned)length)
  CLIENT(friend_message_cb)((Tox*)tox_opaque, friend_number, (TOX_MESSAGE_TYPE)type, message, length, user_data);
}

static void processInFragment(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                              size_t length, void *user_data) {
  uint64_t id;
  unsigned partNo, numParts, off, sz;
  uint8_t markerSize = markerParse(message, length,
                                   &id,
                                   &partNo, &numParts, &off, &sz);

  LOG("RECV", "friend=%u id="FID" length=%u partNo=%u numParts=%u off=%u sz=%u",
    friend_number, id, (unsigned)length, partNo, numParts, off, sz)
  dbInsertInboundFragment((void*)tox,
                          friend_number, type,
                          id, partNo, numParts, off, sz,
                          message + markerSize, length - markerSize,
                          getCurrTimeMs(),
                          messageReady,
                          user_data);
}

//
// periodic
//

void doPeriodic(Tox *tox) {
  //LOG("PERIODIC", "tm="FTM" initializedApi=%d initializedDb=%d", getCurrTimeMs(), initializedApi, initializedDb)
  if (!initializedApi || !initializedDb)
    return;
  // send
  //compressReceipts();
  resendExpiredReceipts(tox);
  sendMore(tox);
  // db
  dbPeriodic();
}


//
// Our API
//

ToxcoreApi MY(initialize_api)(const ToxcoreApi *api) {
  utilInitialize();
  LOG("INIT", "initialize")
  ToxcoreApi MY(toxcore_api);
  // save the base API
  base_toxcore_api = *api;
  // overload API
  MY(toxcore_api) = *api;
  MY(toxcore_api).tox_new = MY(new);
  MY(toxcore_api).tox_kill = MY(kill);
  MY(toxcore_api).tox_friend_send_message = MY(friend_send_message);
  MY(toxcore_api).tox_callback_friend_read_receipt = MY(callback_friend_read_receipt);
  MY(toxcore_api).tox_callback_friend_message = MY(callback_friend_message);
  initializedApi = 1;
  if (initializedApi && initializedDb)
    initialize();
  return MY(toxcore_api);
}

void MY(initialize_db)(sqlite3 *db, ToxDefragmenterDbLockCb lockCb, ToxDefragmenterDbUnlockCb unlockCb, void *user_data) {
  utilInitialize();
  LOG("INIT", "initialize")
  dbInitialize(db, lockCb, unlockCb, user_data);
  initializedDb = 1;
  if (initializedApi && initializedDb)
    initialize();
}

void MY(initialize_db_inmemory)() {
  utilInitialize();
  LOG("INIT", "initialize")
  dbInitializeInMemory();
  initializedDb = 1;
  if (initializedApi && initializedDb)
    initialize();
}

void MY(uninitialize)() {
  LOG("INIT", "finalize")
  uninitialize();
  dbUninitialize();
  utilUninitialize();
  initializedApi = 0;
  initializedDb = 0;
  base_toxcore_api = (ToxcoreApi){0};
}

int MY(is_receipt_pending)(uint32_t receipt) {
  msg_outbound *msg = msgsOutbound;
  if (msg)
    do {
      if (msg->receipt == receipt)
        return 1;
      msg = msg->next;
    } while (msg != msgsOutbound);
  return 0;
}

void MY(set_parameters)(unsigned maxMessageLength,
                        unsigned fragmentsAtATime,
                        unsigned receiptExpirationTimeMs,
                        uint32_t receiptRangeLo, uint32_t receiptRangeHi) {
  if (initializedApi || initializedDb)
    WARNING("parameters should be set in uninitialized state\n")
  if (maxMessageLength <= markerMaxSizeBytes(INT_MAX, INT_MAX))
    WARNING("invalid maxMessageLength=%u in parameters, min value is %u\n", maxMessageLength, markerMaxSizeBytes(INT_MAX, INT_MAX))
  params = (struct params){maxMessageLength, fragmentsAtATime, receiptExpirationTimeMs, receiptRangeLo, receiptRangeHi};
}


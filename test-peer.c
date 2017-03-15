//
// Copyright © 2017 by Yuri Victorovich. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sqlite3.h>
#include <tox/tox.h>
#include "tox-defragmenter.h"

#define CK(cmd) \
  { \
    int res = 0; \
    if ((res = (cmd)) != 0) { \
      printf("ERROR cmd=%s failed with error %s (%d)\n", #cmd, strerror(errno), errno); \
      abort(); \
    } \
  }
#define LOG(fmt...) //{fprintf(stderr, "LOG " fmt); fprintf(stderr, "\n");}
#define ABORT {LOG("ABORT @%s:%u", __FUNCTION__, __LINE__) abort();}
#define ERROR(fmt...) {fprintf(stderr, "ERROR: " fmt); fprintf(stderr, "\n"); ABORT}

#define DEFRAG_RECEIPTS_LO 0x10000000
#define DEFRAG_RECEIPTS_HI DEFRAG_RECEIPTS_LO+1000000

typedef struct Tox Tox;

static sqlite3 *sqlite = NULL;
static bool sawIfaceEOF = false;
static bool sawNetEOF = false;
static bool sawNetEndSignal = false;
static unsigned netExpectMessages = 0;
static unsigned netReceivedMessages = 0;
static uint8_t netReceivedReceiptsShort[1024*1024] = {0}; // XXX limit
static uint8_t netReceivedReceiptsLong[1024*1024] = {0}; // XXX limit
static unsigned msgIdIface = 0;
static unsigned msgIdNet = 0;

//
// files
//
typedef struct stream { // a pair of corresponding fd and file objects
  int   fd;    // used for reading
  FILE  *file; // file is only used for writing
} stream;

static stream streamIn, streamOut, streamNet;

//
// usage
//
static void usage() {
  fprintf(stderr, "Usage: ./test-peer dbFname netSocketFname connectOrListen={C,L}\n");
  fprintf(stderr, "                   paramMaxMessageLength paramFragmentsAtATime paramReceiptExpirationTimeMs\n");
  exit(1);
}

//
// packet queues
//

typedef struct packet { // dual-purpose: represents message or receipt
  struct packet *next;
  uint8_t       *msg;             // msg: data
  size_t         msgLength;       // msg: length
  unsigned       msgId;           // msg: id
  unsigned       receipt;         // rcpt: number=msgId
  unsigned       cntMsgEndSignal; // end: message count to expect
} packet;
static packet *ifaceOutBegin = NULL;
static packet *ifaceOutEnd = NULL;
static packet *netOutBegin = NULL;
static packet *netOutEnd = NULL;

static void packetAppend(packet *r, packet **lstBegin, packet **lstEnd) {
  LOG("packetAppend >>> %p %p", *lstBegin, *lstEnd)
  if (!*lstBegin) {
    *lstBegin = *lstEnd = r;
  } else {
    (*lstEnd)->next = r;
    *lstEnd = r;
  }
  LOG("packetAppend <<< %p %p", *lstBegin, *lstEnd)
}

static packet* packetPickFront(packet **lstBegin, packet **lstEnd) { // ASSUME non-empty list
  packet *p = *lstBegin;
  if (p->next)
    *lstBegin = p->next;
  else
    *lstBegin = *lstEnd = NULL;
  return p;
}

static packet* packetCreateMessage(const uint8_t *msg, size_t length, unsigned msgId) {
  packet *p = malloc(sizeof(packet));
  *p = (packet){.next = NULL, .msg = malloc(length), .msgLength = length, .msgId=msgId, .receipt = 0};
  memcpy(p->msg, msg, length);
  return p;
}

static packet* packetCreateReceipt(unsigned receipt) {
  packet *p = malloc(sizeof(packet));
  *p = (packet){.next = NULL, .msg = NULL, .receipt = receipt};
  return p;
}

static packet* packetCreateEndSignal(unsigned cntMsg) {
  packet *p = malloc(sizeof(packet));
  *p = (packet){.next = NULL, .msg = NULL, .receipt = 0, .cntMsgEndSignal = cntMsg};
  return p;
}

static void packetDelete(packet *p) {
  if (p->msg)
    free(p->msg);
  free(p);
}

//
// utils
//
static unsigned max2(unsigned i1, unsigned i2) {return i1 > i2 ? i1 : i2;}
static unsigned max3(unsigned i1, unsigned i2, unsigned i3) {return max2(max2(i1, i2), i3);}

static char readChar(stream *s) {
  char ch;
  int n;
  switch ((n=read(s->fd, &ch, 1))) {
  case 1:
    LOG("readChar->%c (stream=%p)", ch, s)
    return ch;
  case 0:
    LOG("readChar->EOF (stream=%p)", s)
    return 0; // EOF
  case -1:
    ERROR("readChar got ERROR error=%s", strerror(errno))
  default:
    ABORT
  }
}

static void skipChar(stream *s, char skip) {
  if (readChar(s) != skip)
    ERROR("skip char is wrong")
}

static unsigned readUInt(stream *s, char skip) {
  unsigned u = 0;
  while (true) {
    char ch = readChar(s);
    if (ch == skip)
      break;
    if (!isdigit(ch))
      ERROR("expect a digit but got '%c'", ch)
    u *= 10;
    u += ch - '0';
  }
  LOG("readUInt->%u (stream=%p)", u, s)
  return u;
}

static char* readString(stream *s, char skip) {
  unsigned sz = readUInt(s, ' ');
  char *buf = malloc(sz+1);
  if (read(s->fd, buf, sz) != sz)
    ERROR("can't read a string of length %u", sz)
  buf[sz] = 0;
  skipChar(s, skip);
  return buf;
}

static int openSocket(const char *sockName, char connectOrListen) {
  struct sockaddr_un address;
  int fd, fd1;
  int res;
  
  fd = socket(PF_UNIX, SOCK_STREAM, 0);
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, sockName);

  switch (connectOrListen) {
  case 'C': {
    while (true) {
      res = connect(fd, (struct sockaddr*)&address, sizeof(address));
      if (res && errno == ENOENT) {
        usleep(50*1000); // 50 ms
        continue;
      }
      CK(res)
      break;
    }
    return fd;
  } case 'L': {
    CK(bind(fd, (struct sockaddr*)&address, sizeof(address)))
    CK(listen(fd, 1))
    struct sockaddr sa;
    socklen_t len = sizeof(sa);
    fd1 = accept(fd, &sa, &len);
    if (fd1 < 0)
      ERROR("accept error")
    close(fd);
    return fd1;
  } default:
    ERROR("argument error, expected C or L, got %c", connectOrListen)
  }
}

static bool receivedAllReceipts() {
  unsigned cnt = 0;
  for (int i = 0; i <= msgIdIface && cnt < msgIdIface; i++) {
    if (netReceivedReceiptsShort[i])
      cnt++;
    if (netReceivedReceiptsLong[i])
      cnt++;
  }
  return cnt == msgIdIface;
}

//
// saved network-side callbacks
//
static tox_friend_read_receipt_cb *cb_friend_read_receipt = NULL;
static tox_friend_message_cb *cb_friend_message_cb = NULL;

//
// base Tox iface (net side)
//
static void base_callback_friend_read_receipt(Tox *tox, tox_friend_read_receipt_cb *callback) {
  cb_friend_read_receipt = callback;
}
static void base_callback_friend_message(Tox *tox, tox_friend_message_cb *callback) {
  cb_friend_message_cb = callback;
}
static TOX_CONNECTION base_friend_get_connection_status(const Tox *tox, uint32_t friend_number, TOX_ERR_FRIEND_QUERY *error) {
  return TOX_CONNECTION_UDP;
}
static uint32_t base_friend_send_message(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                        size_t length, TOX_ERR_FRIEND_SEND_MESSAGE *error) {
  msgIdNet++;
  packetAppend(packetCreateMessage(message, length, msgIdNet), &netOutBegin, &netOutEnd);
  LOG("base_friend_send_message: length=%lu msgIdNet=%u appended to the outbound Net queue\n", length, msgIdNet)
  return msgIdNet;
}
static ToxcoreApi apiBase = {.tox_callback_friend_read_receipt = base_callback_friend_read_receipt,
                             .tox_callback_friend_message = base_callback_friend_message,
                             .tox_friend_get_connection_status = base_friend_get_connection_status,
                             .tox_friend_send_message = base_friend_send_message
                            };
// front Tox iface
static ToxcoreApi apiFront;

//
// front callback handlers
//
static void front_friend_message(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                 size_t length, void *user_data) {
  packetAppend(packetCreateMessage(message, length, 0/*msgId*/), &ifaceOutBegin, &ifaceOutEnd);
  netReceivedMessages++;
}

static void front_read_receipt(Tox *tox, uint32_t friend_number, uint32_t message_id, void *user_data) {
  if (message_id < DEFRAG_RECEIPTS_LO)
    netReceivedReceiptsShort[message_id] = 1;
  else
    netReceivedReceiptsLong[message_id - DEFRAG_RECEIPTS_LO] = 1;
  packetAppend(packetCreateReceipt(message_id), &ifaceOutBegin, &ifaceOutEnd);
}

//
// front iface event handlers
//
static void onAnyWR(bool sendMsgId, packet *p, stream *s) {
  LOG("sending pkt=%p into the stream %p", p, s)
  if (p->msg)
    if (sendMsgId)
      fprintf(s->file, "M %u %lu %.*s\n", p->msgId, p->msgLength, (int)p->msgLength, p->msg);
    else
      fprintf(s->file, "M %lu %.*s\n", p->msgLength, (int)p->msgLength, p->msg);
  else if (p->receipt)
    fprintf(s->file, "R %u\n", p->receipt);
  else
    fprintf(s->file, "E %u\n", p->cntMsgEndSignal);
  fflush(s->file);
  packetDelete(p);
}
static bool isIfaceWR() {
  return ifaceOutBegin != NULL;
}
static void onIfaceWR(stream *s) {
  onAnyWR(false/*sendMsgId*/, packetPickFront(&ifaceOutBegin, &ifaceOutEnd), s);
}
static bool isIfaceRD() {
  return !sawIfaceEOF; // expect commands unless saw EOF
}

static void onIfaceRD(stream *s) {
  switch (readChar(s)) {
  case 'M': { // message
    skipChar(s, ' ');
    char *msg = readString(s, '\n');
    LOG("IFACE: onIfaceRD read msg=%s", msg)
    int receipt = apiFront.tox_friend_send_message(NULL, 1, TOX_MESSAGE_TYPE_NORMAL, (const uint8_t*)msg, strlen(msg), NULL);
    ++msgIdIface;
    free(msg);
    if (receipt == 0)
      ERROR("Failed to send the message #%u of length=%lu", msgIdIface, strlen(msg))
    LOG("IFACE: SENT msgNum=%u receipt=%u", msgIdIface, receipt)
    break;
  } case 'E': {
    skipChar(s, '\n');
    LOG("IFACE: got the end signal\n");
    sawIfaceEOF = true;
    packetAppend(packetCreateEndSignal(msgIdIface), &netOutBegin, &netOutEnd);
    break;
  } default:
    ABORT
  }
}

//
// net iface event handlers
//
static bool isNetWR() {
  LOG("isNetWR netOutBegin=%p", netOutBegin)
  return netOutBegin != NULL;
}
static void onNetWR(stream *s) {
  LOG(">>> onNetWR %p %p", netOutBegin, netOutEnd)
  onAnyWR(true/*sendMsgId*/, packetPickFront(&netOutBegin, &netOutEnd), s);
  LOG("<<< onNetWR %p %p", netOutBegin, netOutEnd)
}
static bool isNetRD() {
  LOG("isNetRD sawNetEndSignal=%d sawNetEOF=%d netReceivedMessages=%u netExpectMessages=%u receivedAllReceipts=%u",
    sawNetEndSignal, sawNetEOF, netReceivedMessages, netExpectMessages, receivedAllReceipts())
  return !sawNetEndSignal || !sawNetEOF || netReceivedMessages < netExpectMessages || !receivedAllReceipts();
    // expect packets unless saw EOF or still have packets or receipts to receive
}
static void onNetRD(stream *s) {
  LOG(">>> onNetRD")
  switch (readChar(s)) {
  case 'M': { // message: M msgId sz msg nl
    skipChar(s, ' ');
    unsigned msgId = readUInt(s, ' ');
    char *msg = readString(s, '\n');
    LOG("NET: onNetRD >>> read msg=%s len=%lu", msg, strlen(msg))
    cb_friend_message_cb(NULL, 1, TOX_MESSAGE_TYPE_NORMAL, (const uint8_t*)msg, strlen(msg), NULL/*user_data*/);
    LOG("NET: onNetRD <<< passed to cb msg=%s len=%lu", msg, strlen(msg))
    free(msg);
    // send receipt back to the sender
    packetAppend(packetCreateReceipt(msgId), &netOutBegin, &netOutEnd);
    break;
  } case 'R': { // receipt: R msgId nl
    skipChar(s, ' ');
    unsigned msgId = readUInt(s, '\n');
    LOG("NET: onNetRD >>> got the receipt %u\n", msgId);
    cb_friend_read_receipt(NULL, 1/*friend*/, msgId, NULL/*user_data*/);
    LOG("NET: onNetRD <<< got the receipt %u\n", msgId);
    break;
  } case 'E': { // end signal: E netExpectMessages nl
    skipChar(s, ' ');
    sawNetEndSignal = true;
    netExpectMessages = readUInt(s, '\n');
    break;
  } case 0: {
    if (!sawNetEndSignal)
      ERROR("Net EOF but no end signal")
    sawNetEOF = true;
    break;
  } default:
    ABORT
  }
  LOG("<<< onNetRD")
}
static bool needContinue() {
  LOG("needContinue: %d %d %d %u %u %p %p %u -> %d",
    sawIfaceEOF, sawNetEndSignal, sawNetEOF, netReceivedMessages, netExpectMessages, ifaceOutBegin, netOutBegin, receivedAllReceipts(),
        !sawIfaceEOF ||
        !sawNetEndSignal || netReceivedMessages < netExpectMessages || !receivedAllReceipts() ||
        ifaceOutBegin || netOutBegin
  )
  return !sawIfaceEOF ||
         !sawNetEndSignal || netReceivedMessages < netExpectMessages || !receivedAllReceipts() ||
         ifaceOutBegin || netOutBegin;
}
static void onTimeout() {
}

typedef bool (*IsXX)();
typedef void (*OnXX)(stream *s);
typedef void (*OnTimeout)();

static void loop3(stream *s1, IsXX isWR1, OnXX onWR1, IsXX isRD1, OnXX onRD1,
                  stream *s2, IsXX isWR2, OnXX onWR2, IsXX isRD2, OnXX onRD2,
                  stream *s3, IsXX isWR3, OnXX onWR3, IsXX isRD3, OnXX onRD3,
                  OnTimeout onTimeout,
                  IsXX needToContinue) {
  int res;
  do {
    fd_set rdSet, wrSet;
    FD_ZERO(&rdSet);
    FD_ZERO(&wrSet);
    if (isRD1 && isRD1()) FD_SET(s1->fd, &rdSet);
    if (isRD2 && isRD2()) FD_SET(s2->fd, &rdSet);
    if (isRD3 && isRD3()) FD_SET(s3->fd, &rdSet);
    if (isWR1 && isWR1()) FD_SET(s1->fd, &wrSet);
    if (isWR2 && isWR2()) FD_SET(s2->fd, &wrSet);
    if (isWR3 && isWR3()) FD_SET(s3->fd, &wrSet);
    res = select(max3(s1->fd, s2->fd, s3->fd)+1, &rdSet, &wrSet, NULL, NULL);
    if (res > 0) {
      if (FD_ISSET(s1->fd, &rdSet)) onRD1(s1);
      if (FD_ISSET(s2->fd, &rdSet)) onRD2(s2);
      if (FD_ISSET(s3->fd, &rdSet)) onRD3(s3);
      if (FD_ISSET(s1->fd, &wrSet)) onWR1(s1);
      if (FD_ISSET(s2->fd, &wrSet)) onWR2(s2);
      if (FD_ISSET(s3->fd, &wrSet)) onWR3(s3);
    } else {
      CK(res)
    }
  } while (needToContinue());
}

static void loop(IsXX needToContinue) {
  loop3(&streamIn,  NULL,      NULL,      isIfaceRD, onIfaceRD,
        &streamOut, isIfaceWR, onIfaceWR, NULL,      NULL,
        &streamNet, isNetWR,   onNetWR,   isNetRD,   onNetRD,
        onTimeout,
        needToContinue);
}


//
// MAIN
//

int main(int argc, char *argv[]) {
  if (argc != 7)
    usage();
  streamIn  = (stream){.fd = STDIN_FILENO,  .file = stdin};
  streamOut = (stream){.fd = STDOUT_FILENO, .file = stdout};
  // open
  streamNet.fd = openSocket(argv[2]/*netSocketFname*/, argv[3][0] /*doConnect*/);
  streamNet.file = fdopen(streamNet.fd, "w");
  LOG("pid#%d: connected, streamIn=%p streamOut=%p streamNet=%p", getpid(), &streamIn, &streamOut, &streamNet)

  // params
  tox_defragmenter_set_parameters(atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), DEFRAG_RECEIPTS_LO, DEFRAG_RECEIPTS_HI);

  // initialize interface
  if (argv[1][0]) {
    CK(sqlite3_open(argv[1]/*dbFname*/, &sqlite))
    apiFront = tox_defragmenter_initialize_api(&apiBase);
    tox_defragmenter_initialize_db(sqlite, NULL, NULL, NULL);
  } else {
    tox_defragmenter_initialize_db_inmemory();
  }
  apiFront.tox_callback_friend_message(NULL, front_friend_message);
  apiFront.tox_callback_friend_read_receipt(NULL, front_read_receipt);

  // loop
  loop(&needContinue);

  // finish
  tox_defragmenter_uninitialize();

  // close
  if (sqlite)
    sqlite3_close(sqlite);
  fclose(streamNet.file);
  close(streamNet.fd);

  return 0;
}


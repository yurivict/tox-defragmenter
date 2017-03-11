//
// Copyright © 2017 by Yuri Victorovich. All rights reserved.
//

#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>
#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>
#include "marker.h"

static const uint8_t markerChar[3] = {0xe2, 0x80, 0x8b}; // ZERO WIDTH SPACE' (U+200B), 3 bytes in UTF8 representation
// frag_id is always 13 digits long, milliseconds timestamp
static const int szMarkerChar = sizeof(markerChar);
static const int szTm = 13, szIntMin = 1, szIntMax = 10, nInts = 4;

#define false 0
#define true 1
typedef size_t U;

// internal declarations

static int numDigits(unsigned i);
static int isMarkerChar(const uint8_t *s);
static unsigned parseUInt32(const uint8_t *str, U off);
static uint64_t parseUInt64(const uint8_t *str, U off);
static int markerParseFields(const uint8_t *message, size_t length, U* fldOff, U* fldSz);

// functions

uint8_t markerMaxSizeBytes(unsigned numParts, unsigned msgSize) {
  int numPartsDigits = numDigits(numParts);
  int msgSizeDigits = numDigits(msgSize);
  return szMarkerChar+szTm+1+numPartsDigits+1+numPartsDigits+1+msgSizeDigits+1+msgSizeDigits+szMarkerChar;
}

uint8_t markerPrint(uint64_t id, unsigned partNo, unsigned numParts, unsigned off, unsigned sz, uint8_t *marker) {
  return sprintf((char*)marker, "%c%c%c%"PRIu64"|%u|%u|%u|%u%c%c%c",
           markerChar[0], markerChar[1], markerChar[2],
           id,
           partNo,
           numParts,
           off,
           sz,
           markerChar[0], markerChar[1], markerChar[2]);
}

int markerExists(const uint8_t *message, size_t length) {
  U fldOff[nInts] = {0};
  U fldSz[nInts] = {0};

  return markerParseFields(message, length, fldOff, fldSz);
}

uint8_t markerParse(const uint8_t *message, size_t length,
                    uint64_t *id,
                    unsigned *partNo, unsigned *numParts, unsigned *off, unsigned *sz) {
  U fldOff[nInts] = {0};
  U fldSz[nInts] = {0};

  if (!markerParseFields(message, length, fldOff, fldSz))
    return 0; // not a marker

  *id          = parseUInt64(message, szMarkerChar);
  *partNo      = parseUInt32(message, fldOff[0]);
  *numParts    = parseUInt32(message, fldOff[1]);
  *off         = parseUInt32(message, fldOff[2]);
  *sz          = parseUInt32(message, fldOff[3]);

  return fldOff[3] + fldSz[3] + szMarkerChar;
}

// internal definitions

static int numDigits(unsigned i) {
  int ndigits = 1;
  while (i /= 10)
    ndigits++;
  return ndigits;
}

static int isMarkerChar(const uint8_t *s) {
  return s[0] == markerChar[0] &&
         s[1] == markerChar[1] &&
         s[2] == markerChar[2];
}

static unsigned parseUInt32(const uint8_t *str, U off) {
  char *ep;
  return strtoul((char*)str+off, &ep, 10);
}

static uint64_t parseUInt64(const uint8_t *str, U off) {
  char *ep;
  return strtoull((char*)str+off, &ep, 10);
}

static int markerParseFields(const uint8_t *message, size_t length, U* fldOff, U* fldSz) {
  if (length <= szMarkerChar+szTm+nInts*(1+szIntMin)+szMarkerChar) // nInts integer fields with separators
    return false;
  if (!isMarkerChar(message) || message[szMarkerChar+szTm] != '|')
    return false;
  // parse timestamp
  for (int i = szMarkerChar; i < szMarkerChar+szTm; i++) {
    if (!isdigit(message[i]))
      return false;
  }
  // parse partNo, numParts, off
  size_t p = szMarkerChar+szTm+1;
  for (int f = 0; f < nInts; f++) {
    fldOff[f] = p;
    for (int i = p; i < p+szIntMax && i < length; i++) {
      if (!isdigit(message[i]))
        break;
      fldSz[f]++;
    }
    if (fldSz[f] == 0 || p+fldSz[f]+1 >= length || ((f < nInts-1) ? message[p+fldSz[f]] != '|' : !isMarkerChar(message+p+fldSz[f])))
      return false;
    p += fldSz[f] + 1;
  }

  return true;
}


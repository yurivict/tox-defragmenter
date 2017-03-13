
#include "util.h"
#include <stdio.h>
#include <sys/time.h>
#include <stdarg.h>

struct timeval tmInitialized = {0};

void utilInitialize() {
  struct timeval tm;
  gettimeofday(&tmInitialized, NULL);
}

void utilUninitialize() {
  tmInitialized = (struct timeval){0, 0};
}

void utilLog(const char *function, const char *section, const char *fmt, ...) {
  char msg[4*1024], *p = msg;
  va_list vl;
  struct timeval tm;

  gettimeofday(&tm, NULL);
  tm.tv_sec -= tmInitialized.tv_sec;
  if (tm.tv_usec >= tmInitialized.tv_usec) {
    tm.tv_usec -= tmInitialized.tv_usec;
  } else {
    tm.tv_usec += 1000000;
    tm.tv_usec -= tmInitialized.tv_usec;
    tm.tv_sec -= 1;
  }

  va_start(vl, fmt);
  p += sprintf(p, "[%u.%03u] LOG(Defragmenter.%s): %s: ", (unsigned)tm.tv_sec, (unsigned)tm.tv_usec/1000, section, function);
  p += vsprintf(p, fmt, vl);
  va_end(vl);

  printf("%s\n", msg);
}


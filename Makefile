
SRCS=		tox-defragmenter.c database.c marker.c util.c
HEADERS=	tox-defragmenter.h database.h marker.h util.h common.h sqlite-interface.h
OBJS=		$(SRCS:.c=.o)
LIB_SO=		libtox-defragmenter.so
LIB_A=		libtox-defragmenter.a
ALL_O=		tox-defragmenter-all.o

TOX_HEADERS?=   /usr/local/include
CFLAGS_OPT?=	-O3
CFLAGS+=	$(CFLAGS_OPT)
CFLAGS+=	-I$(TOX_HEADERS)
CFLAGS+=	-fPIC
CFLAGS+=	-Wall

all: build

build: $(LIB_SO) $(LIB_A)

$(LIB_SO): $(ALL_O)
	$(CC) -shared -o $@ $< $(CFLAGS) $(LDFLAGS)

$(LIB_A): $(ALL_O)
	rm -f $@
	ar rcs $@ $<

$(ALL_O): $(SRCS) $(HEADERS) Makefile
	$(CC) -c $(SRCS) $(CFLAGS)
	ld $(OBJS) -Ur $(CFLAGS_OPT) -o $@.ld.o
	objcopy --localize-hidden $@.ld.o $@
	rm $@.ld.o

install:
	mkdir -p $(DESTDIR)/$(PREFIX)/include $(DESTDIR)/$(PREFIX)/lib
	cp tox-defragmenter.h $(DESTDIR)/$(PREFIX)/include/tox-defragmenter.h
	cp $(LIB_SO) $(LIB_A) $(DESTDIR)/$(PREFIX)/lib/

clean:
	rm -f $(OBJS) $(ALL_O) $(LIB_SO) $(LIB_A) test-peer

run-regression-tests: tests
	./test.sh

tests: test-peer

test-peer: test-peer.c $(LIB_A) Makefile
	$(CC) $(CFLAGS) -o $@ $< $(LIB_A) -L/usr/local/lib -lsqlite3

.PHONY: all build install clean tests run-regression-tests

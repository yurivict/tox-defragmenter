
SRCS=		tox-defragmenter.c database.c marker.c util.c
HEADERS=	tox-defragmenter.h database.h marker.h util.h sqlite-interface.h
OBJS=		$(SRCS:.c=.o)
DEFRAG_LIB_SO=	libtox-defragmenter.so
DEFRAG_LIB_A=	libtox-defragmenter.a
DEFRAG_ALL_O=	tox-defragmenter-all.o

TOX_HEADERS?=   /usr/local/include
CFLAGS_OPT?=	-O3
CFLAGS+=	-I$(TOX_HEADERS)
CFLAGS+=	-fPIC
CFLAGS+=	$(CFLAGS_OPT)

all: build

build: $(DEFRAG_LIB_SO) $(DEFRAG_LIB_A)

$(DEFRAG_LIB_SO): $(DEFRAG_ALL_O)
	$(CC) -shared -o $@ $< $(CFLAGS) $(LDFLAGS)

$(DEFRAG_LIB_A): $(DEFRAG_ALL_O)
	rm -f $@ && \
	ar rcs $@ $<

$(DEFRAG_ALL_O): $(SRCS)
	$(CC) -c $(SRCS) $(CFLAGS) && \
	ld $(OBJS) -Ur $(CFLAGS_OPT) -o $@.ld.o && \
	objcopy --localize-hidden $@.ld.o $@ && \
	rm $@.ld.o

$(DEFRAG_ALL_O): $(HEADERS) Makefile

install:
	mkdir -p $(DESTDIR)/$(PREFIX)/include $(DESTDIR)/$(PREFIX)/lib
	cp tox-defragmenter.h $(DESTDIR)/$(PREFIX)/include/tox-defragmenter.h
	cp $(DEFRAG_LIB_SO) $(DEFRAG_LIB_A) $(DESTDIR)/$(PREFIX)/lib/

clean:
	rm -f $(OBJS) $(DEFRAG_ALL_O) $(DEFRAG_LIB_SO) $(DEFRAG_LIB_A)

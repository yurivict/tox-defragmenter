
SRCS=		tox-defragmenter.c database.c marker.c
HEADERS=	tox-defragmenter.h database.h marker.h sqlite-interface.h
DEFRAG_LIB=	libtox-defragmenter.so

CFLAGS?=	-I/usr/local/include -O3 -fPIC
VERS_SCRIPT=	tox-defragmenter.version
VERS_FLAGS=	-Wl,--version-script,"$(VERS_SCRIPT)"

build: $(DEFRAG_LIB)

$(DEFRAG_LIB): $(SRCS)
	$(CC) $(VERS_FLAGS) -shared -o $@ $(SRCS) $(CFLAGS) $(LDFLAGS)

$(DEFRAG_LIB): $(HEADERS) $(VERS_SCRIPT) Makefile

install:
	mkdir -p $(DESTDIR)/include $(DESTDIR)/lib
	cp tox-defragmenter.h $(DESTDIR)/include/tox-defragmenter.h
	cp $(DEFRAG_LIB) $(DESTDIR)/lib/$(DEFRAG_LIB)

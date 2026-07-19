# liszt - GNU make required (gmake on FreeBSD).
ifeq ($(filter else-if,$(.FEATURES)),)
$(error GNU make is required; use gmake on BSD systems)
endif

include config.mk

VERSION := $(shell sed -n 's/^LISZT_VERSION="\(.*\)"/\1/p' configure)

SRC = \
	src/main.c \
	src/options.c \
	src/plan.c \
	src/dirread.c \
	src/entry.c \
	src/sortkey.c \
	src/human.c \
	src/idcache.c \
	src/timefmt.c \
	src/colors.c \
	src/icons.c \
	src/gitignore.c \
	src/git.c \
	src/quote.c \
	src/uniwidth.c \
	src/layout.c \
	src/emit.c \
	src/recurse.c \
	src/util.c \
	src/sys/dir.c \
	src/sys/xstat.c \
	src/sys/scan.c \
	src/sys/thread.c

OBJ = $(SRC:.c=.o)
DEP = $(OBJ:.o=.d)

CPPFLAGS += -I. -Isrc -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 $(EXTRA_CPPFLAGS)
CFLAGS ?= -O2
CFLAGS += -std=c11 -pthread -Wall -Wextra -Werror -Wpedantic -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wwrite-strings
LDFLAGS += -pthread

PREFIX ?= /usr/local

all: config.h liszt lz

liszt: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

# lz is the same binary under a shorter name (argv[0] changes nothing but
# diagnostics). Copy in-tree; symlink at install time.
lz: liszt
	@cmp -s liszt lz 2>/dev/null || cp -f liszt lz

config.mk config.h: configure
	./configure

%.o: %.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

check: unit golden tree gitcheck fuzz-smoke perf-smoke

unit: all
	sh tests/unit/run.sh

golden: all
	sh tests/golden/run.sh || test $$? -eq 77

tree: all
	sh tests/tree/run.sh || test $$? -eq 77

gitcheck: all
	sh tests/git/run.sh || test $$? -eq 77

fuzz-smoke: all
	FUZZ_TRIALS=25 sh tests/fuzz/run.sh || test $$? -eq 77
	FUZZ_TRIALS=10 sh tests/fuzz/run-tree.sh

fuzz: all
	sh tests/fuzz/run.sh
	sh tests/fuzz/run-tree.sh

perf-smoke: all
	sh bench/run-smoke.sh || test $$? -eq 77

sanitize:
	$(MAKE) clean
	$(MAKE) check CFLAGS='$(CFLAGS) $(SANITIZE_FLAGS)' LDFLAGS='$(LDFLAGS) $(SANITIZE_FLAGS)'
	$(MAKE) clean
	$(MAKE) all

# ThreadSanitizer over the parallel stat phase: goldens exercise it via
# LISZT_PARALLEL_MIN=1 in the harness env when set here.
tsan:
	$(MAKE) clean
	$(MAKE) all CFLAGS='$(CFLAGS) -fsanitize=thread' LDFLAGS='$(LDFLAGS) -fsanitize=thread'
	LISZT_PARALLEL_MIN=1 sh tests/golden/run.sh
	LISZT_PARALLEL_MIN=1 sh tests/tree/run.sh
	FUZZ_TRIALS=10 LISZT_PARALLEL_MIN=1 sh tests/fuzz/run.sh || test $$? -eq 77
	FUZZ_TRIALS=10 LISZT_PARALLEL_MIN=1 sh tests/fuzz/run-tree.sh
	$(MAKE) clean
	$(MAKE) all

dist:
	git archive --format=tar.gz --prefix=liszt-$(VERSION)/ \
		-o liszt-$(VERSION).tar.gz HEAD

distcheck: dist
	rm -rf build/distcheck
	mkdir -p build/distcheck
	tar -xzf liszt-$(VERSION).tar.gz -C build/distcheck
	cd build/distcheck/liszt-$(VERSION) && ./configure && $(MAKE) && sh tests/unit/run.sh

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 0755 liszt $(DESTDIR)$(PREFIX)/bin/liszt
	ln -sf liszt $(DESTDIR)$(PREFIX)/bin/lz
	install -m 0644 doc/liszt.1 $(DESTDIR)$(PREFIX)/share/man/man1/liszt.1
	install -m 0644 doc/lz.1 $(DESTDIR)$(PREFIX)/share/man/man1/lz.1

clean:
	rm -f liszt lz $(OBJ) $(DEP)

distclean: clean
	rm -f config.h config.mk liszt-*.tar.gz
	rm -rf build

.PHONY: all check unit golden tree gitcheck fuzz fuzz-smoke perf-smoke sanitize dist \
	distcheck install clean distclean

-include $(DEP)

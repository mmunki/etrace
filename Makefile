CC ?= cc
ARCH_NAME ?= $(shell uname -m)
PREFIX ?= /usr/local
STATIC ?= 0

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -I.
LDFLAGS ?=
LDLIBS ?=

ifeq ($(STATIC),1)
LDFLAGS += -static
endif

CORE_SRCS = main.c etrace.c syscalls.c render.c recorder.c replayer.c dump.c
OBJS = $(CORE_SRCS:.c=.o)
BIN = etrace
HEADERS = etrace.h etrace_format.h syscalls.h syscall_names.h

.PHONY: all clean distclean test release install

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

test_crash: test_crash.c
	$(CC) $(CFLAGS) -pthread $(LDFLAGS) -o $@ $<

test: $(BIN) test_crash
	ETRACE_BIN=./$(BIN) TEST_CRASH=./test_crash bash tests/run_tests.sh

release: clean
	@mkdir -p dist
	$(MAKE) STATIC=1 all
	cp -f $(BIN) dist/etrace-$(ARCH_NAME)
	@echo "built dist/etrace-$(ARCH_NAME)"

install: $(BIN)
	install -D -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/etrace

clean:
	rm -f $(OBJS) $(BIN) test_crash

distclean: clean
	rm -rf dist

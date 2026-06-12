# SPDX-License-Identifier: GPL-3.0-or-later
CC     ?= cc
WARN    = -Wall -Wextra
BASE    = -O2 -g -std=c11 $(WARN)

# Each test has its own include path to avoid proto.h collision:
# firmware's proto.h lives in components/core/include,
# CLI's proto.h lives in tools/esp-ctl/src.
CFLAGS_TOK = $(BASE) -I../components/core/include -Istubs
CFLAGS_PRT = $(BASE) -I../tools/esp-ctl/src

LDLIBS = -lcrypto

CORE_SRC = ../components/core/src/cmd.c
CLI_SRC  = ../tools/esp-ctl/src/proto.c \
           ../tools/esp-ctl/src/crypto.c

BINS = test_tokenizer test_proto test_discover

all: $(BINS)
	@echo ""
	./test_tokenizer
	@echo ""
	./test_proto
	@echo ""
	./test_discover

test_tokenizer: test_tokenizer.c $(CORE_SRC)
	$(CC) $(CFLAGS_TOK) $^ -o $@

test_proto: test_proto.c $(CLI_SRC)
	$(CC) $(CFLAGS_PRT) $^ -o $@ $(LDLIBS)

test_discover: test_discover.c ../tools/esp-ctl/src/discover.c
	$(CC) $(BASE) -o $@ $<

clean:
	$(RM) $(BINS)

.PHONY: all clean

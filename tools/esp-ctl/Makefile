# SPDX-License-Identifier: GPL-3.0-or-later
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE
LDLIBS  := -lcrypto

SRC := src/esp-ctl.c src/proto.c src/crypto.c src/profile.c src/discover.c
OBJ := $(SRC:.c=.o)
BIN := esp-ctl

PREFIX ?= /usr/local

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(OBJ) $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

.PHONY: all clean install

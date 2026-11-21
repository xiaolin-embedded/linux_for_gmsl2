# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local
DESTDIR ?=

all: gmsl-reg

gmsl-reg: gmsl-reg.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f gmsl-reg

install: gmsl-reg
	install -D -m 0755 gmsl-reg $(DESTDIR)$(PREFIX)/bin/gmsl-reg

.PHONY: all clean install

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PKGS     = sdl2 SDL2_image
PREFIX  ?= /usr/local

CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

shuzi: shuzi.c
	$(CC) $(CFLAGS) -o $@ shuzi.c $(LDLIBS)

install: shuzi
	install -Dm755 shuzi $(DESTDIR)$(PREFIX)/bin/shuzi

clean:
	rm -f shuzi

.PHONY: install clean

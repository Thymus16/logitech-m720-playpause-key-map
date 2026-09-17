CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
HARDENING = -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE
LDFLAGS ?=

.PHONY: all test sanitize clean
all: m720-playpause

m720-playpause: m720-playpause.c desktop.c desktop.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(HARDENING) m720-playpause.c desktop.c $(LDFLAGS) -pie -Wl,-z,relro,-z,now -o $@

media-check: tools/media-check.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(HARDENING) $< $(LDFLAGS) -pie -Wl,-z,relro,-z,now -o $@

tests/test: tests/test.c m720-playpause.c desktop.c desktop.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(HARDENING) tests/test.c desktop.c $(LDFLAGS) -pie -o $@

tests/test-desktop: tests/test-desktop.c desktop.c desktop.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(HARDENING) tests/test-desktop.c $(LDFLAGS) -pie -o $@

test: m720-playpause tests/test tests/test-desktop
	./tests/test
	./tests/test-desktop
	./m720-playpause --help

sanitize:
	$(CC) $(CPPFLAGS) -O1 -g $(WARNINGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test.c desktop.c -o tests/test-sanitize
	./tests/test-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARNINGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test-desktop.c -o tests/test-desktop-sanitize
	./tests/test-desktop-sanitize

clean:
	$(RM) m720-playpause media-check tests/test tests/test-sanitize tests/test-desktop tests/test-desktop-sanitize

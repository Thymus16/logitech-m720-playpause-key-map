CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
HARDENING = -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE
LDFLAGS ?=

.PHONY: all test sanitize clean
all: m720-playpause

m720-playpause: m720-playpause.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(HARDENING) $< $(LDFLAGS) -pie -Wl,-z,relro,-z,now -o $@

media-check: tools/media-check.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(HARDENING) $< $(LDFLAGS) -pie -Wl,-z,relro,-z,now -o $@

tests/test: tests/test.c m720-playpause.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(HARDENING) $< $(LDFLAGS) -pie -o $@

test: m720-playpause tests/test
	./tests/test
	./m720-playpause --help

sanitize:
	$(CC) $(CPPFLAGS) -O1 -g $(WARNINGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test.c -o tests/test-sanitize
	./tests/test-sanitize

clean:
	$(RM) m720-playpause media-check tests/test tests/test-sanitize

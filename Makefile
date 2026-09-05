CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)
GLIB_LIBS  := $(shell pkg-config --libs glib-2.0)

# Hardening: this program is routinely run as root.
HARDEN_CFLAGS  := -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 \
                  -fPIE -Wformat -Wformat-security
HARDEN_LDFLAGS := -pie -Wl,-z,relro,-z,now,-z,noexecstack

CFLAGS  += $(GTK_CFLAGS) -Isrc -std=gnu11 $(HARDEN_CFLAGS)
LDFLAGS += $(HARDEN_LDFLAGS)
LDLIBS  += $(GTK_LIBS) -lm

SRC := src/main.c src/ui.c src/tc.c src/profile.c src/cli.c
OBJ := $(SRC:.c=.o)

all: testlag

testlag: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/lag.h src/tc.h src/profile.h src/ui.h src/cli.h
	$(CC) $(CFLAGS) -c $< -o $@

# Headless backend test (no GTK needed).
test: test/test_tc

test/test_tc: test/test_tc.c src/tc.c src/profile.c src/lag.h src/tc.h src/profile.h
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ test/test_tc.c src/tc.c src/profile.c $(GLIB_LIBS) -lm

# Run the backend test inside a private network namespace (needs user ns).
test-run: test
	unshare -r -n ./test/test_tc

clean:
	rm -f testlag $(OBJ) test/test_tc

.PHONY: all test test-run clean

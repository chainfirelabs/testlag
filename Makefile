CC      ?= cc
GLIB_COMPILE_RESOURCES ?= glib-compile-resources
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

SRC := src/askpass.c src/main.c src/ui.c src/tc.c src/profile.c src/cli.c src/resources.c
OBJ := $(SRC:.c=.o)

all: testlag

testlag: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/lag.h src/tc.h src/profile.h src/ui.h src/cli.h src/askpass.h
	$(CC) $(CFLAGS) -c $< -o $@

src/resources.c: resources.xml logo-banner.png
	$(GLIB_COMPILE_RESOURCES) --sourcedir=. --generate-source --target=$@ resources.xml

# Headless backend test (no GTK needed).
test: test/test_tc test/test_sudo

test/test_tc: test/test_tc.c src/tc.c src/profile.c src/lag.h src/tc.h src/profile.h
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ test/test_tc.c src/tc.c src/profile.c $(GLIB_LIBS) -lm

test/test_sudo: test/test_sudo.c src/tc.c src/tc.h src/lag.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/test_sudo.c src/tc.c $(GLIB_LIBS) -lm \
	    -Wl,--wrap=geteuid,--wrap=g_spawn_sync,--wrap=g_spawn_async_with_pipes

test/test_askpass: test/test_askpass.c src/askpass.c src/askpass.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/test_askpass.c src/askpass.c $(LDLIBS)

test/test_ui: src/resources.c test/test_ui.c src/ui.c src/ui.h src/tc.c src/tc.h src/profile.c src/profile.h src/lag.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/test_ui.c src/tc.c src/profile.c src/resources.c $(LDLIBS) \
	    -Wl,--wrap=tc_apply_interface,--wrap=tc_get_state_text

# Requires a graphical test display (Xvfb or Broadway).
test-gui: test/test_askpass test/test_ui
	./test/test_askpass
	./test/test_ui

# Run the backend test inside a private network namespace (needs user ns).
test-run: test
	./test/test_sudo
	unshare -r -n ./test/test_tc

test-ingress: testlag
	unshare -r -n python3 test/test_ingress.py

clean:
	rm -f testlag $(OBJ) src/resources.c test/test_tc test/test_sudo test/test_askpass test/test_ui

.PHONY: all test test-run test-gui test-ingress clean

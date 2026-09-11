# Host-side unit tests for sleepd.
#
# These compile the units under test straight out of src/ against small stub
# headers (tests/unit/stubs/) that replace the PmLogLib logging macros and
# the luna-service2 clock helpers, so the only host dependency is glib-2.0.
#
#   make -C tests/unit          # build + run
#   make -C tests/unit build    # build only
#   make -C tests/unit clean

TOP       := ../..
BUILDDIR  := out

CC        ?= cc
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS   := $(shell pkg-config --libs glib-2.0)

CFLAGS  += -std=gnu99 -g -O1 -Wall -Werror=implicit-function-declaration \
           -fsanitize=address,undefined -fno-omit-frame-pointer \
           -Istubs -I$(TOP)/include/internal $(GLIB_CFLAGS)
LDFLAGS += -fsanitize=address,undefined
LDLIBS  += $(GLIB_LIBS) -lpthread

# config.c resolves its paths from compile-time macros; route the conf dir
# through an env var so each test run gets a fresh temp dir.
CONFIG_DEFS := \
    -DWEBOS_INSTALL_DEFAULTCONFDIR='g_getenv("SLEEPD_TEST_CONF_DIR")' \
    -DWEBOS_INSTALL_LOCALSTATEDIR='"/tmp/sleepd-test-var"'

TESTS := $(BUILDDIR)/test_wait $(BUILDDIR)/test_timersource $(BUILDDIR)/test_config

all: check

build: $(TESTS)

$(BUILDDIR):
	mkdir -p $@

$(BUILDDIR)/test_wait: test_wait.c $(TOP)/src/utils/wait.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/test_timersource: test_timersource.c $(TOP)/src/utils/timersource.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/test_config: test_config.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(CONFIG_DEFS) $(LDFLAGS) -o $@ $< $(LDLIBS)

check: $(TESTS)
	@fail=0; \
	for t in $(TESTS); do \
	    echo "== $$t"; \
	    ./$$t || fail=1; \
	done; \
	exit $$fail

clean:
	rm -rf $(BUILDDIR)

.PHONY: all build check clean

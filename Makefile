# cogs-fujinet-lib: fujinet-lib with a CoGS card bus, Apple II (cc65).
#
# Output: build/fujinet-apple2-<ver>.lib plus the fujinet-lib headers
# copied beside it, so an application built with MekkoGX or the older
# fujinet-lib.mk picks it up with  FUJINET_LIB=<path to this checkout>.
#
# Upstream fujinet-lib (GPL-3) is the submodule fujinet-lib/; its common/
# and include/ are used as-is. Only src/ is ours.

CC65     ?= cl65
AR65     ?= ar65
TARGET   ?= apple2
UPSTREAM  = fujinet-lib
VERSION  := $(shell awk '/FNLIB_VERSION_FULL/{ gsub(/"/, "", $$3); sub(/^v/, "", $$3); sub(/-dev$$/, "", $$3); print $$3 }' $(UPSTREAM)/include/fujinet-version.h)
BUILD     = build
OBJDIR    = $(BUILD)/obj
LIB       = $(BUILD)/fujinet-$(TARGET)-$(VERSION).lib

OPT      ?= -Os
CFLAGS    = $(OPT) -t $(TARGET) -DBUILD_APPLE2 -I $(UPSTREAM)/include -I src $(EXTRA_CFLAGS)

COMMON_SRC = $(wildcard $(UPSTREAM)/common/*.c)
BUS_SRC    = $(wildcard src/*.c)
HEADERS    = $(wildcard $(UPSTREAM)/include/*.h) $(wildcard $(UPSTREAM)/include/*.inc)

OBJS = $(patsubst $(UPSTREAM)/common/%.c,$(OBJDIR)/common_%.o,$(COMMON_SRC)) \
       $(patsubst src/%.c,$(OBJDIR)/bus_%.o,$(BUS_SRC))

.PHONY: all clean headers
all: $(LIB) headers

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/common_%.o: $(UPSTREAM)/common/%.c | $(OBJDIR)
	$(CC65) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/bus_%.o: src/%.c | $(OBJDIR)
	$(CC65) $(CFLAGS) -c -o $@ $<

$(LIB): $(OBJS)
	rm -f $@
	$(AR65) a $@ $(OBJS)
	@echo "built $@"

headers: | $(OBJDIR)
	cp $(HEADERS) $(BUILD)/
	@# MekkoGX defines FNLIB_VERSION_FULL from the .lib file name ("5.1.1")
	@# and the dev-tree header says "v5.1.1-dev"; cc65 rejects the mismatch.
	@# Stamp the installed header with the release form the file name has.
	sed -i '' 's/^\(#define FNLIB_VERSION_FULL\).*/\1 "$(VERSION)"/' $(BUILD)/fujinet-version.h

clean:
	rm -rf $(BUILD)

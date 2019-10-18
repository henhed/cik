#!/usr/bin/make -f

SHELL = /bin/sh
CC = gcc
INSTALL = install -c
INSTALLDATA = install -c -m 644

SRC_PATH = src
BUILD_PATH = build
BIN_PATH = $(BUILD_PATH)/bin
BIN_NAME = cik
CONF_NAME = cik.conf
SD_NAME = cik.service
SRC_EXT = c

SOURCES = $(shell find $(SRC_PATH) -name '*.$(SRC_EXT)' | sort -k 1nr | cut -f2-)
OBJECTS = $(SOURCES:$(SRC_PATH)/%.$(SRC_EXT)=$(BUILD_PATH)/%.o)
DEPS = $(OBJECTS:.o=.d)

COMPILER_FLAGS = -std=c11 -Wall -Wextra -Werror -ggdb -D_GNU_SOURCE -DHAVE_SYSTEMD=1
INCLUDES = -I include/
LIBS = -pthread $(shell pkg-config --libs libsystemd)
LDFLAGS =

prefix = /usr
bindir = $(prefix)/bin
etcdir = /etc/cik
sysdir = /lib/systemd/system

.PHONY: release
release: export CCFLAGS := $(CCFLAGS) $(COMPILER_FLAGS) -Winline -O3 -DDEBUG=0
release: dirs
	@$(MAKE) all

.PHONY: debug
debug: export CCFLAGS := $(CCFLAGS) $(COMPILER_FLAGS) -DDEBUG=1
debug: dirs
	@$(MAKE) all

.PHONY: dirs
dirs:
	@echo "Creating directories"
	@mkdir -p $(dir $(OBJECTS))
	@mkdir -p $(BIN_PATH)

.PHONY: clean
clean:
	@echo "Deleting $(BIN_NAME) symlink"
	@$(RM) $(BIN_NAME)
	@echo "Deleting directories"
	@$(RM) -r $(BUILD_PATH)
	@$(RM) -r $(BIN_PATH)

.PHONY: all
all: $(BIN_PATH)/$(BIN_NAME)
	@echo "Making symlink: $(BIN_NAME) -> $<"
	@$(RM) $(BIN_NAME)
	@ln -s $(BIN_PATH)/$(BIN_NAME) $(BIN_NAME)

.PHONY: run
run: debug
	@./$(BIN_NAME) ./cik.conf

.PHONY: install
install: release
	@$(INSTALL) -D $(BIN_PATH)/$(BIN_NAME) $(DESTDIR)$(bindir)/$(BIN_NAME)
	@$(INSTALLDATA) -D ./$(CONF_NAME) $(DESTDIR)$(etcdir)/$(CONF_NAME)
	@$(INSTALLDATA) -D ./$(SD_NAME) $(DESTDIR)$(sysdir)/$(SD_NAME)

$(BIN_PATH)/$(BIN_NAME): $(OBJECTS)
	@echo "Linking: $@"
	$(CC) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $@

-include $(DEPS)

$(BUILD_PATH)/%.o: $(SRC_PATH)/%.$(SRC_EXT)
	@echo "Compiling: $< -> $@"
	$(CC) $(CCFLAGS) $(INCLUDES) -c $< -o $@

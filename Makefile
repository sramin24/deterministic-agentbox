# Output goes to out/, deliberately NOT named "build" -- a phony target
# also named "build" alongside a same-named output directory creates a
# circular dependency in GNU Make ("Circular build/x.o <- build dependency
# dropped"). See Part 0.8 of the design notes.

CC := gcc
CFLAGS := -O3 -fPIC -Wall -Wextra -std=c11
LDFLAGS := -ldl

SRC_DIR := src/c
OUT_DIR := out

ENGINE_SRCS := $(SRC_DIR)/agentbox_ns.c $(SRC_DIR)/agentbox_cow.c $(SRC_DIR)/agentbox_proc.c $(SRC_DIR)/agentbox_core.c
ENGINE_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OUT_DIR)/%.o,$(ENGINE_SRCS))
HEADER := $(SRC_DIR)/agentbox.h

LIB := $(OUT_DIR)/libagentbox.so
EXEC_BIN := $(OUT_DIR)/agentbox-exec
TEST_C_BIN := $(OUT_DIR)/test_c_engine

.PHONY: all build test-c clean

all: build

build: $(LIB) $(EXEC_BIN)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/%.o: $(SRC_DIR)/%.c $(HEADER) | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(ENGINE_OBJS) | $(OUT_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $(ENGINE_OBJS) $(LDFLAGS)

$(EXEC_BIN): $(SRC_DIR)/agentbox_exec_main.c $(ENGINE_OBJS) $(HEADER) | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/agentbox_exec_main.c $(ENGINE_OBJS) $(LDFLAGS)

$(TEST_C_BIN): tests/test_c_engine.c $(ENGINE_OBJS) $(HEADER) | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ tests/test_c_engine.c $(ENGINE_OBJS) $(LDFLAGS)

# Requires the one-time `out/agentbox-exec --install-apparmor <path>` root
# step first (see README) -- this target itself must run unprivileged, or
# it would mask real unprivileged-path bugs.
test-c: $(TEST_C_BIN) $(EXEC_BIN)
	./$(TEST_C_BIN)

clean:
	rm -rf $(OUT_DIR)

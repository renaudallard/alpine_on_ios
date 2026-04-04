CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -g -Iinclude -D_GNU_SOURCE
LDFLAGS = -lpthread -lm

SRCS = $(wildcard src/*.c)
ASM_SRCS = $(wildcard src/*.S)
OBJS = $(SRCS:src/%.c=build/%.o) $(ASM_SRCS:src/%.S=build/%.o)
TEST_SRCS = $(filter-out tests/test_run.c tests/test_interp.c tests/test_interactive_fork.c tests/test_multi_fork.c,$(wildcard tests/*.c))
TEST_OBJS = $(TEST_SRCS:tests/%.c=build/tests/%.o)

.PHONY: all clean test

all: build/libemu.a

build/libemu.a: $(OBJS)
	ar rcs $@ $^

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: src/%.S | build
	$(CC) -c -o $@ $<

build/tests/%.o: tests/%.c | build/tests
	$(CC) $(CFLAGS) -c -o $@ $<

build/test_runner: $(TEST_OBJS) build/libemu.a
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) -Lbuild -lemu $(LDFLAGS)

test: build/test_runner
	./build/test_runner

build:
	mkdir -p build

build/tests:
	mkdir -p build/tests

clean:
	rm -rf build

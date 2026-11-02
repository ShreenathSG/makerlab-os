CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS = -lsqlite3 -lpthread -lm

.PHONY: all run test clean
all: build/makerlab
build:
	mkdir -p build
build/makerlab: src/server.c src/core.c src/core.h | build
	$(CC) $(CFLAGS) src/server.c src/core.c -o $@ $(LDLIBS)
build/core_test: tests/core_test.c src/core.c src/core.h | build
	$(CC) $(CFLAGS) -Isrc tests/core_test.c src/core.c -o $@ -lm
run: all
	./build/makerlab
test: all build/core_test
	./build/core_test
	python3 tests/integration.py
clean:
	rm -rf build

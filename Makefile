CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Iinclude
LDFLAGS ?=

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)

all: build/hxc

build:
	@mkdir -p build

build/hxc: $(OBJS) | build
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

test: build/hxc
	@sh ./tests/run.sh
size: build/hxc
	@./build/hxc build examples/hola.hxe -o build/hola && ./build/hxc size build/hola
	@sz=$$(wc -c < build/hola); if [ $$sz -gt 12288 ]; then echo "FALLO: $$sz bytes > 12288"; exit 1; fi; echo "OK: $$sz <= 12288 bytes"

clean:
	rm -rf build $(OBJS) $(DEPS)

.PHONY: all clean test size
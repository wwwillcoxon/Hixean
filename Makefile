CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Iinclude
LDFLAGS ?=
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=all

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

# El corpus entero con el compilador instrumentado. Un error de memoria en el
# front-end no sale como un error de memoria, sino como un diagnostico raro en
# el programa de otra persona, asi que hay que verlo aqui. Las arenas no se
# liberan nunca a proposito, por eso detect_leaks esta apagado.
asan:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory CC="$(CC)" CFLAGS="-O1 -g -std=c11 $(SAN) -Iinclude" LDFLAGS="$(SAN)"
	@ASAN_OPTIONS=detect_leaks=0 sh tests/run.sh || (echo "FALLO: la suite no pasa bajo sanitizers"; $(MAKE) --no-print-directory clean; exit 1)
	@$(MAKE) --no-print-directory clean
	@echo "ok     corpus limpio bajo address+undefined"

# Entradas rotas: el parser promete recuperacion de errores (ADR 0002), y una
# promesa asi solo se sostiene con entradas que nadie eligio.
fuzz:
	@sh tests/fuzz.sh

manual:
	@echo "docs/manual.html: abriend en un navegador busqueda, tema oscuro y tabla de errores con filtro"
	@echo "para PDF: el boton 'Guardar PDF' usa el dialogo de impresión del navegador"
	@ls -l docs/manual.html

clean:
	rm -rf build $(OBJS) $(DEPS)

.PHONY: all clean test size asan fuzz manual
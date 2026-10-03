CC      ?= cc
# Los flags que se pueden exigir sin ruido. -Wshadow y -Wcast-qual sacaron
# llaves escondidas: un const descartado al construir los vectores de argumentos.
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wcast-qual \
           -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wvla \
           -Wwrite-strings -Wformat=2 -Wno-unused-parameter -Iinclude
LDFLAGS ?=
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=all
VERSION := $(shell sed -n 's/^const char \*HX_VERSION = "\(.*\)";/\1/p' src/common.c)
PLAT    ?= $(shell sh tools/plat.sh)

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

# El paquete que se publica en la release de GitHub: binario, licencia,
# changelog, gramatica, manual y ejemplos, mas su SHA256.
dist: build/hxc
	@sh tools/dist.sh $(VERSION) $(PLAT)

manual:
	@echo "docs/manual.html: abriend en un navegador busqueda, tema oscuro y tabla de errores con filtro"
	@echo "para PDF: el boton 'Guardar PDF' usa el dialogo de impresión del navegador"
	@ls -l docs/manual.html

clean:
	rm -rf build $(OBJS) $(DEPS)

.PHONY: all clean test size asan fuzz dist manual
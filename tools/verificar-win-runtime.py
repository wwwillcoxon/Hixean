#!/usr/bin/env python3
"""Compila el bloque de runtime que se emite para Windows.

Hay partes del runtime que solo se emiten en una plataforma —el reloj de Windows, el
net de POSIX— y aqui no se compilan nunca: el `if defined(_WIN32)` que las elige se
evalua al compilar hxc, no al compilar el programa. Un error en ese trozo no sale en
Linux, no sale en macOS y sale en el CI de Windows, que es donde se descubre tarde y
con un mensaje que habla de una constante que no existe.

Esto extrae el bloque tal cual se emite, le pone delante tipos de Windows de
mentira y lo compila con los mismos avisos que el resto del proyecto. Si un dia hay
un SystemRoot de verdad en el CI, esto deja de ser util y se puede borrar.
"""
import os
import re
import subprocess
import sys
import tempfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STUBS = """#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
typedef union { struct { int64_t low; int64_t high; } u; int64_t QuadPart; } LARGE_INTEGER;
typedef unsigned long DWORD;
typedef void *HMODULE;
typedef int64_t (*FARPROC)(void);
/* El contador de rendimiento va desde el arranque del sistema, y una maquina que
   lleva encendida un rato ya ha acumulado marcas suficientes para que multiplicar
   por mil millones se salga del int64: un ano a 10 MHz son 3.15e20 marcas, y el
   int64 llega a 9.2e18. Con el stub de valores pequenos que habia antes eso no se
   veía, y el reloj de Windows devolvia cero en el CI sin que aqui se notase nada.
   El stub arranca con un ano de uptime. */
#define HX_STUB_UPTIME_TICKS 315360000000000LL
static inline int QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 10000000; return 1; }
static inline int QueryPerformanceCounter(LARGE_INTEGER *c) {
  c->QuadPart = HX_STUB_UPTIME_TICKS;
  return 1;
}
static inline void Sleep(DWORD ms) { (void)ms; }
static inline HMODULE GetModuleHandleA(const char *n) { (void)n; return 0; }
static inline FARPROC GetProcAddress(HMODULE m, const char *n) { (void)m; (void)n; return 0; }
typedef struct { int64_t seg; int64_t nsec; } hx_timespec;
"""

# Los mismos avisos que el Makefile, mas -Wcast-function-type que viene con -Wextra
# y que es el que prohibe convertir FARPROC en la firma que quiere BCryptGenRandom.
FLAGS = ["-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wshadow",
         "-Wcast-qual", "-Wstrict-prototypes", "-Wmissing-prototypes",
         "-Wcast-function-type"]


def bloque(nombre):
    """El texto que se emite para el runtime `nombre`, tal cual."""
    with open(os.path.join(RAIZ, "src", "emit.c"), encoding="utf-8") as f:
        fuente = f.read()
    i = fuente.find("static const char *%s =" % nombre)
    if i < 0:
        return None
    j = fuente.index('"}\\n";', i) + len('"}\\n";')
    partes = re.findall(r'"((?:[^"\\]|\\.)*)"', fuente[i:j])
    return "".join(partes).replace("\\n", "\n").replace('\\"', '"')


def main():
    fallos = 0
    for nombre in ("HX_TIME_WIN",):
        texto = bloque(nombre)
        if texto is None:
            print("FALLO: no existe el bloque %s en src/emit.c" % nombre)
            return 1
        # windows.h no existe aqui: lo que se comprueba es el bloque, no la cabecera.
        texto = texto.replace('#include <windows.h>', "").replace('#include <time.h>', "")
        main = ('int main(void) {\n'
                '  unsigned char b[4];\n'
                '  int64_t ns = hx_time_ns();\n'
                '  /* Un ano de uptime son 3.1536e16 nanosegundos. Se comprueba el orden\n'
                '     de magnitud y no el numero: lo que importa es que no se haya ido\n'
                '     por el desbordamiento, y un stub con el reloj del runner daria un\n'
                '     numero distinto cada vez. */\n'
                '  int ok = ns > 31000000000000000LL && ns < 32000000000000000LL;\n'
                '  if (!ok) printf("el reloj dio %lld y deberia dar unos 3.15e16\\n",\n'
                '                 (long long)ns);\n'
                '  return (hx_time_random(b, 4) && ok) ? 0 : 1;\n}\n')
        fuente = STUBS + texto + main
        with tempfile.TemporaryDirectory() as d:
            c = os.path.join(d, "bloque.c")
            exe = os.path.join(d, "bloque")
            with open(c, "w", encoding="utf-8") as f:
                f.write(fuente)
            p = subprocess.run(["cc"] + FLAGS + ["-o", exe, c],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            if p.returncode != 0:
                print("FALLO: el runtime de Windows no compila:\n%s" % p.stderr)
                fallos += 1
                continue
            # Y ademas se ejecuta, y se comprueba que el reloj da un numero de la
            # magnitud del uptime: 32 dias estan en 2.7e15 nanosegundos. Un reloj que
            # se sale del int64 da negativo o cero, y con un valor de 12345 —que es lo
            # que tenia el stub antes— eso no se ve.
            p = subprocess.run([exe], capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            if p.returncode != 0:
                print("FALLO: el runtime de Windows no funciona (rc=%d)" % p.returncode)
                # El programa dice que le pasa: sin esto la puerta dice «no funciona»
                # y no dice cómo, que es el mismo problema que solve el primer día.
                if p.stdout.strip():
                    for linea in p.stdout.strip().split("\n"):
                        print("       %s" % linea.strip())
                fallos += 1
                continue
    if fallos:
        return 1
    print("ok     el runtime de Windows que se emite para otro sistema compila y funciona")
    return 0


if __name__ == "__main__":
    sys.exit(main())

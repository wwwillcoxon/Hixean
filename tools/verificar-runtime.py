#!/usr/bin/env python3
"""Las longitudes fijas del runtime no se calculan, se escriben a mano.

hx_write(2, "hx: error: ", 11) copia 11 bytes; si alguien pone 12, el programa
escribe un byte de mas y sale un NUL delante del mensaje. No se ve en el
codigo generado a ojo, y en un binario de 8 KB es exactamente el tipo de
detalle que no aparece en un test de hola mundo.

    tools/verificar-runtime.py
"""

import re
import sys

ESCAPES = {"n": 1, "r": 1, "t": 1, "0": 1, "\\": 1, '"': 1, "'": 1, "a": 1, "b": 1, "f": 1, "v": 1}


def largo(literal):
    """Bytes que ocupa un literal de C, con los escapes contados como uno."""
    total = 0
    i = 0
    while i < len(literal):
        c = literal[i]
        if c == "\\" and i + 1 < len(literal):
            nxt = literal[i + 1]
            if nxt == "x":
                total += 1
                i += 3
                continue
            if nxt == "u":
                while i < len(literal) and literal[i] != "{":
                    i += 1
                i += 1
                total += 1
                continue
            total += ESCAPES.get(nxt, 1)
            i += 2
            continue
        total += 1
        i += 1
    return total


def main():
    with open("src/emit.c", encoding="utf-8") as f:
        fuente = f.read()
    # dentro de las cadenas del runtime todo va escapado: "\\n" aqui es "\n" en el
    # C generado, y ahi "\n" son 2 caracteres que en C son 1 byte
    plano = fuente.replace(chr(92) * 2, chr(92)).replace(chr(92) + chr(34), chr(34))
    patron = re.compile(r'hx_(?:write|out)\(\s*(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*(\d+)')
    revistos = 0
    fallos = []
    for m in patron.finditer(plano):
        literal, declarado = m.group(2), int(m.group(3))
        revistos += 1
        real = largo(literal)
        if real != declarado:
            fallos.append((m.group(0), real, declarado))
    for texto, real, declarado in fallos:
        print("FALLO: %s -> %d bytes reales, %d declarados" % (texto[:80], real, declarado))
    if not revistos:
        print("FALLO: no se ha encontrado ninguna llamada hx_write/hx_out: el runtime ha cambiado")
        return 1
    if fallos:
        return 1
    print("ok     %d longitudes fijas del runtime cuadran con sus literales" % revistos)
    return 0


if __name__ == "__main__":
    sys.exit(main())
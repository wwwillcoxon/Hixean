#!/usr/bin/env python3
"""Comprueba que el PDF del sitio se ve.

    python3 tools/verificar-pdf.py site/guia-programar.pdf

Tres cosas, y las tres son fallos que ya han pasado:

1. **Que no haya texto invisible.** En PDF, `rg` fija el color de relleno, y ese
   color es el que usan tambien las letras. Un rectángulo de fondo sin devolver
   el color a negro deja todo el texto posterior en gris claro sobre blanco: se
   dibuja y no se ve. Pasaba con los bloques de codigo y con los avisos, y eran
   3792 fragmentos. Aqui se recorre el flujo de operadores en orden, como lo ve
   el visor, y se avisa de cada texto que sale con un relleno claro encima.

2. **Que no haya texto fuera de la pagina.** Una linea de codigo larga que se
   sale por la derecha pierde las ultimas letras sin decir nada.

3. **Que el archivo sea un PDF de verdad**: empieza por %PDF-, acaba en %%EOF y
   el numero de objetos de la xref cuadra con el numero de objetos escritos.

Lo que este script NO puede comprobar es si la tipografia que usa el visor tiene
los glifos: para eso estan las fuentes base 14, que estan en todos los visores por
definicion.
"""

import os
import re
import sys
import zlib

ANCHO_PAG = 595.0
ALTO_PAG = 842.0
MARGEN = 6.0   # tolerancia: un texto puede asomar un pelo al margen

# Anchos de Helvetica, en milesimas de em. Para medir cuanto ocupa un texto; es
# una aproximacion y solo importa para detectar lo que se sale de la pagina.
AN = {" ": 278, "!": 278, '"': 355, "#": 556, "$": 556, "%": 889, "&": 667,
      "'": 191, "(": 333, ")": 333, "*": 389, "+": 584, ",": 278, "-": 333,
      ".": 278, "/": 278, "0": 556, "1": 556, "2": 556, "3": 556, "4": 556,
      "5": 556, "6": 556, "7": 556, "8": 556, "9": 556, ":": 278, ";": 278,
      "<": 584, "=": 584, ">": 584, "?": 556, "@": 1015, "[": 278, "]": 278,
      "_": 556, "a": 556, "b": 556, "c": 500, "d": 556, "e": 556, "f": 278,
      "g": 556, "h": 556, "i": 222, "j": 222, "k": 500, "l": 222, "m": 833,
      "n": 556, "o": 556, "p": 556, "q": 556, "r": 333, "s": 500, "t": 278,
      "u": 556, "v": 500, "w": 722, "x": 500, "y": 500, "z": 500, "A": 667,
      "B": 667, "C": 722, "D": 722, "E": 667, "F": 611, "G": 778, "H": 722,
      "I": 278, "J": 500, "K": 667, "L": 556, "M": 833, "N": 722, "O": 778,
      "P": 667, "Q": 778, "R": 722, "S": 667, "T": 611, "U": 722, "V": 667,
      "W": 944, "X": 667, "Y": 667, "Z": 611}


def ancho(txt, fuente, tam):
    base = 600 if fuente == "F3" else AN.get(txt[0] if txt else " ", 556)
    total = 0
    for c in txt:
        total += 600 if fuente == "F3" else AN.get(c, 556)
    return total * tam / 1000.0


def main():
    if len(sys.argv) != 2:
        print("uso: verificar-pdf.py <fichero.pdf>")
        return 2
    ruta = sys.argv[1]
    if not os.path.exists(ruta):
        print("FALLO: no existe %s" % ruta)
        return 1
    crudo = open(ruta, "rb").read()
    fallos = []

    if not crudo.startswith(b"%PDF-"):
        fallos.append("el archivo no empieza por %PDF-")
    if not crudo.rstrip().endswith(b"%%EOF"):
        fallos.append("el archivo no acaba en %%EOF")

    # objetos declarados en la xref frente a objetos escritos
    m = re.search(rb"startxref\s+(\d+)", crudo)
    if m:
        off = int(m.group(1))
        xr = crudo[off:off + 40]
        if not xr.startswith(b"xref"):
            fallos.append("startxref no apunta a la tabla xref")
        else:
            enc = re.match(rb"xref\s+0 (\d+)", xr)
            if enc:
                declarados = int(enc.group(1))
                escritos = len(re.findall(rb"(?m)^\d+ 0 obj$", crudo))
                if declarados != escritos + 1:
                    fallos.append("la xref declara %d objetos y hay %d escritos"
                                  % (declarados - 1, escritos))

    flujos = []
    for mm in re.finditer(rb"stream\n(.*?)\nendstream", crudo, re.S):
        try:
            flujos.append(zlib.decompress(mm.group(1)))
        except zlib.error:
            flujos.append(mm.group(1))   # sin comprimir: tambien vale para mirar

    op_rg = re.compile(rb"([\d.]+) ([\d.]+) ([\d.]+) rg")
    op_txt = re.compile(rb"BT /(F\d) ([\d.]+) Tf 1 0 0 1 ([\d.]+) ([\d.]+) Tm \((.*?)\) Tj ET")

    invisibles = 0
    fuera = 0
    textos = 0
    for i, flujo in enumerate(flujos, 1):
        color = None
        eventos = []
        for m in op_rg.finditer(flujo):
            eventos.append((m.start(), "rg", m))
        for m in op_txt.finditer(flujo):
            eventos.append((m.start(), "txt", m))
        for pos, clase, m in sorted(eventos, key=lambda e: e[0]):
            if clase == "rg":
                color = tuple(float(m.group(i)) for i in (1, 2, 3))
                continue
            textos += 1
            fuente = m.group(1).decode()
            tam = float(m.group(2))
            x, y = float(m.group(3)), float(m.group(4))
            txt = m.group(5)
            txt = txt.replace(b"\\(", b"(").replace(b"\\)", b")").replace(b"\\\\", b"\\")
            txt = txt.decode("cp1252", "replace")
            # 1. invisible: el relleno se puso claro y nadie lo devolvio a negro
            if color and min(color) > 0.45:
                invisibles += 1
                if invisibles <= 3:
                    fallos.append("pagina %d: texto invisible, relleno %s: %r"
                                  % (i, color, txt[:40]))
            # 2. fuera de la pagina
            if y < 0 or y > ALTO_PAG or x < -MARGEN or x + ancho(txt, fuente, tam) > ANCHO_PAG + MARGEN:
                fuera += 1
                if fuera <= 3:
                    fallos.append("pagina %d: texto fuera de la pagina (x=%.0f y=%.0f): %r"
                                  % (i, x, y, txt[:40]))
            color = None

    for f in fallos:
        print("FALLO: " + f)
    if fallos:
        return 1
    print("ok     %s: %d paginas, %d textos, ninguno invisible ni fuera de pagina"
          % (os.path.basename(ruta), len(flujos), textos))
    return 0


if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
"""Genera los PNG del sitio: el favicon para navegadores viejos y la imagen que
se ve al compartir un enlace.

No hay conversor de SVG ni Pillow en este repositorio, y meter una dependencia
solo para cuatro rectangulos seria una dependencia entera que mantener. Asi que
esto escribe el PNG a mano: zlib esta en la biblioteca estandar y un PNG son
cabecera, chunks y un filtro 0 por linea.

La fuente es un mapa de bits de 5x7 dibujado a mano, no una tipografia: sale
pixelada a proposito, que es como se ve el resto del sitio (monoespaciada,
terminal) y asi que no hace falta ni fuentes nimeasuretexto.

    python3 tools/generar-iconos.py           # escribe los PNG
    python3 tools/generar-iconos.py --ver      # ademas los enseña en ASCII
"""
import struct
import sys
import zlib
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
SITIO = RAIZ / "site"

FONDO = (0x0F, 0x0E, 0x0C)
PAPEL = (0x1B, 0x1A, 0x15)
LINEA = (0x33, 0x30, 0x2A)
TINTA = (0xEF, 0xEA, 0xDE)
SUAVE = (0x8D, 0x86, 0x76)
ACENTO = (0xE0, 0x8B, 0x5E)

# 5x7, una fila por pixel. '#' es tinta. Solo hace falta lo que dice HIXEAN.
GLIFOS = {
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": ["#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "11": [".##..", "#..#.", ".##..", "..#..", ".##..", "#..#.", ".##.."],
}


class Lienzo:
    """Un lienzo de pixeles RGB. Se pinta con rectangulos y con glifos."""

    def __init__(self, w, h, fondo=FONDO):
        self.w, self.h = w, h
        self.px = bytearray(fondo * (w * h))

    def rect(self, x, y, w, h, color):
        """Pinta un rectangulo entero."""
        for j in range(max(0, y), min(self.h, y + h)):
            base = j * self.w
            for i in range(max(0, x), min(self.w, x + w)):
                o = (base + i) * 3
                self.px[o:o + 3] = bytes(color)

    def panel(self, x, y, w, h, relleno=PAPEL, borde=LINEA, grosor=2):
        """Un rectangulo con marco de verdad: relleno y borde de otro color, que
        es lo que hace visible el marco."""
        self.rect(x, y, w, h, relleno)
        self.rect(x, y, w, grosor, borde)
        self.rect(x, y + h - grosor, w, grosor, borde)
        self.rect(x, y, grosor, h, borde)
        self.rect(x + w - grosor, y, grosor, h, borde)

    def texto(self, x, y, cadena, color, escala=1):
        """Dibuja una cadena con la fuente de mapa de bits."""
        for letra in cadena.upper():
            filas = GLIFOS.get(letra, GLIFOS[" "])
            for f, fila in enumerate(filas):
                for c, bit in enumerate(fila):
                    if bit == "#":
                        self.rect(x + c * escala, y + f * escala, escala, escala, color)
            x += 6 * escala

    def ancho_texto(self, cadena, escala=1):
        return len(cadena) * 6 * escala

    def png(self, destino):
        crudo = bytearray()
        for j in range(self.h):
            crudo.append(0)  # filtro 0: ninguno. Con zlib y color de tipo 2
            crudo += self.px[j * self.w * 3:(j + 1) * self.w * 3]

        def chunk(tipo, datos):
            c = struct.pack(">I", len(datos)) + tipo + datos
            return c + struct.pack(">I", zlib.crc32(tipo + datos) & 0xFFFFFFFF)

        cab = struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)
        blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", cab)
                + chunk(b"IDAT", zlib.compress(bytes(crudo), 9))
                + chunk(b"IEND", b""))
        destino.write_bytes(blob)
        return len(blob)


def icono(w, h):
    """El favicon: fondo, marco, y tres barras que son una linea de codigo.

    El diseño esta en un espacio de 64 y se escala, para que el mismo codigo
    sirva para el PNG de 16 como para el de 512 sin numeros magicos."""
    c = Lienzo(w, h)
    d = 64
    k = w / d

    def r(x, y, rw, rh, color):
        c.rect(round(x * k), round(y * k), round(rw * k), round(rh * k), color)

    g = max(1, round(3 * k))
    c.panel(round(6 * k), round(6 * k), round(52 * k), round(52 * k), grosor=g)
    r(15, 26, 22, 5, ACENTO)
    r(41, 26, 9, 5, SUAVE)
    r(15, 37, 13, 5, SUAVE)
    return c


def og():
    """La imagen que se ve al compartir un enlace: 1200x630, que es lo que
    Telegram, Slack, Discord y X piden."""
    w, h = 1200, 630
    c = Lienzo(w, h)
    c.panel(28, 28, w - 56, h - 56, grosor=3)

    # el nombre, grande
    escala = 18
    c.texto(80, 170, "HIXEAN", ACENTO, escala)

    # y debajo, una linea de codigo en version de barras: mismo lenguaje, mismo
    # sitio, sin una tipografia metida a mano
    y = 170 + 7 * escala + 46
    c.rect(80, y, 520, 10, TINTA)
    c.rect(624, y, 180, 10, SUAVE)
    c.rect(80, y + 32, 300, 10, SUAVE)
    c.rect(404, y + 32, 240, 10, ACENTO)
    c.rect(80, y + 64, 420, 10, SUAVE)

    c.texto(80, 462, "COMPILA A C11", SUAVE, 7)
    c.texto(80, 524, "8 896 BYTES", SUAVE, 7)
    return c


def ver(c):
    """Enseña el lienzo en ASCII. Para cuando haya que mirar que se ha dibujado
    bien sin abrir un visor de imagen. Cada color es un caracter, asi que lo
    que se ve a ojo es lo que sale en el PNG."""
    leyenda = {FONDO: " ", TINTA: "O", ACENTO: "#", SUAVE: "o", LINEA: ":", PAPEL: "."}
    for j in range(c.h):
        fila = ""
        for i in range(c.w):
            o = (j * c.w + i) * 3
            fila += leyenda.get((c.px[o], c.px[o + 1], c.px[o + 2]), "?")
        print(fila)


def main():
    if "--ver" in sys.argv:
        print("# acento   o suave   O tinta   : linea   . papel   (negro)")
        ver(icono(64, 64))
        print()
        ver(og())
        return
    for w, h, nombre in ((32, 32, "favicon-32.png"), (16, 16, "favicon-16.png"),
                         (180, 180, "apple-touch-icon.png"), (512, 512, "icon-512.png")):
        n = icono(w, h).png(SITIO / nombre)
        print(f"ok     site/{nombre} ({w}x{h}, {n} bytes)")
    n = og().png(SITIO / "og-hixean.png")
    print(f"ok     site/og-hixean.png (1200x630, {n} bytes)")


if __name__ == "__main__":
    main()
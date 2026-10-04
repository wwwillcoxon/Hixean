#!/usr/bin/env python3
"""Un PDF a partir de markdown, sin dependencias.

Ni pandoc, ni LaTeX, ni wkhtmltopdf, ni reportlab. En este proyecto eso es lo
correcto: si el manual dependiera de una biblioteca que no vive en el
repositorio, solo se podria regenerar en la maquina de alguien.

Que hace, y que no:

- Hace: portada, indice con numeros de pagina, titulos de tres niveles,
  paragrafos, listas con vineta y numeradas, bloques de codigo monoespaciados,
  `codigo` en linea, negrita y cursiva, y pie de pagina.
- No hace: tablas, imagenes, enlaces internos ni HTML. Si aparece algo de eso
  avisa y sigue, porque un PDF a medias sirve mas que un error.

El texto va en las fuentes base 14 (Helvetica, Helvetica-Bold, Helvetica-Oblique
y Courier), que cualquier lector tiene incorporadas. El archivo no lleva fuentes
dentro: son unas decenas de KB en vez de varios megas, y se ve igual en todos los
visores.

    python3 tools/generar-pdf.py docs/guia-programar.md site/guia-programar.pdf
"""

import os
import re
import sys
import zlib

# --- metricas -------------------------------------------------------------
# Anchos de Helvetica en milesimas de em. No son exactos al mil: lo que hace
# falta es que la palabra quepa, y lo que importa aqui es no pasarse de ancho.
HELV = {
    " ": 278, "!": 278, '"': 355, "#": 556, "$": 556, "%": 889, "&": 667,
    "'": 191, "(": 333, ")": 333, "*": 389, "+": 584, ",": 278, "-": 333,
    ".": 278, "/": 278, ":": 278, ";": 278, "<": 584, "=": 584, ">": 584,
    "?": 556, "@": 1015, "[": 278, "\\": 278, "]": 278, "^": 469, "_": 556,
    "`": 333, "{": 334, "|": 260, "}": 334, "~": 584,
    "A": 667, "B": 667, "C": 722, "D": 722, "E": 667, "F": 611, "G": 778,
    "H": 722, "I": 278, "J": 500, "K": 667, "L": 556, "M": 833, "N": 722,
    "O": 778, "P": 667, "Q": 778, "R": 722, "S": 667, "T": 611, "U": 722,
    "V": 667, "W": 944, "X": 667, "Y": 667, "Z": 611,
    "a": 556, "b": 556, "c": 500, "d": 556, "e": 556, "f": 278, "g": 556,
    "h": 556, "i": 222, "j": 222, "k": 500, "l": 222, "m": 833, "n": 556,
    "o": 556, "p": 556, "q": 556, "r": 333, "s": 500, "t": 278, "u": 556,
    "v": 500, "w": 722, "x": 500, "y": 500, "z": 500,
}
for _c in "0123456789":
    HELV[_c] = 556
COURIER = dict((c, 600) for c in
               " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
               "abcdefghijklmnopqrstuvwxyz{|}~")

ANCHO_PAG = 595          # A4 vertical a 72 dpi
ALTO_PAG = 842
MIZQ = 64
MDER = 64
MSUP = 64
MINF = 68
ANCHO = ANCHO_PAG - MIZQ - MDER

CUERPO = 10.5
INTERL = 14.6            # interlineado del cuerpo
COD = 8.8
INTERL_COD = 12.0
TIT1 = 20.0
TIT2 = 14.5
TIT3 = 11.8

FUENTES = [("F1", "Helvetica"), ("F2", "Helvetica-Bold"),
           ("F3", "Courier"), ("F4", "Helvetica-Oblique")]


def ancho_de(txt, fuente):
    tabla = COURIER if fuente == "F3" else HELV
    total = 0
    for c in txt:
        total += tabla.get(c, 600 if fuente == "F3" else 556)
    return total


def a_winansi(txt):
    """Las fuentes base 14 con WinAnsi cubren el castellano con un byte por letra."""
    return txt.encode("cp1252", "replace").decode("cp1252")


def escapar(txt):
    """ parentesis y barra invertida cuentan dentro de una cadena PDF."""
    return a_winansi(txt).replace("\\", r"\\").replace("(", r"\(").replace(")", r"\)")


class Pagina:
    def __init__(self, numero):
        self.numero = numero
        self.ops = []
        self.y = ALTO_PAG - MSUP

    def texto(self, x, y, txt, fuente="F1", tam=CUERPO):
        if txt:
            self.ops.append("BT /%s %.2f Tf 1 0 0 1 %.2f %.2f Tm (%s) Tj ET"
                            % (fuente, tam, x, y, escapar(txt)))

    def regla(self, y, gris=0.85, grosor=0.6):
        self.ops.append("%.2f %.2f %.2f RG %.2f w %.2f %.2f m %.2f %.2f l S"
                        % (gris, gris, gris, grosor, MIZQ, y, ANCHO_PAG - MDER, y))

    def caja(self, x, y, w, h, gris=0.965):
        """Un rectángulo de fondo, y el relleno vuelve a negro.

        Sin ese rg negro, el color se queda puesto: en PDF `rg` fija el color de
        relleno, que es el que usa tambien el texto, asi que todo lo que se
        dibuja despues sale en gris claro sobre blanco, es decir invisible. Le
        pasaba a los bloques de codigo y a los avisos: 3792 fragmentos de texto
        del documento, que se dibujaban pero no se veian."""
        self.ops.append("%.3f %.3f %.3f rg %.2f %.2f %.2f %.2f re f" % (gris, gris, gris, x, y, w, h))
        self.ops.append("0 0 0 rg")


class Doc:
    def __init__(self, meta):
        self.meta = meta
        self.paginas = []
        self.titulos = []          # (nivel, texto, numero_de_pagina)

    # -- gestion de paginas ----------------------------------------------
    def nueva(self):
        p = Pagina(len(self.paginas) + 1)
        self.paginas.append(p)
        return p

    def pag(self):
        return self.paginas[-1]

    def sitio(self, alto):
        """True si queda sitio para `alto` en la pagina actual."""
        return self.pag().y - alto >= MINF

    def Ensure(self, alto):
        if not self.sitio(alto):
            self.nueva()

    # -- bloques ----------------------------------------------------------
    def parrafo(self, trozos, sangria=0.0):
        lineas = []
        cur, curw = [], 0.0
        maxw = ANCHO - sangria
        for txt, fuente, tam in trozos:
            for palabra in re.split(r"(\s+)", txt):
                if not palabra:
                    continue
                w = ancho_de(palabra, fuente) * tam / 1000.0
                if w > maxw:
                    for trozo in self._partir(palabra, fuente, tam, maxw):
                        if cur:
                            lineas.append(cur)
                        lineas.append([(trozo, fuente, tam)])
                        cur, curw = [], 0.0
                    continue
                if curw + w > maxw and cur:
                    lineas.append(cur)
                    cur, curw = [], 0.0
                    if palabra.isspace():
                        continue
                cur.append((palabra, fuente, tam))
                curw += w
        if cur:
            lineas.append(cur)
        for ln in lineas:
            self.Ensure(INTERL)
            p = self.pag()
            p.y -= INTERL
            x = MIZQ + sangria
            for palabra, fuente, tam in ln:
                p.texto(x, p.y + tam * 0.26, palabra, fuente, tam)
                x += ancho_de(palabra, fuente) * tam / 1000.0

    def _partir(self, palabra, fuente, tam, maxw):
        trozos, actual = [], ""
        for c in palabra:
            if ancho_de(actual + c, fuente) * tam / 1000.0 > maxw and actual:
                trozos.append(actual)
                actual = c
            else:
                actual += c
        if actual:
            trozos.append(actual)
        return trozos

    def titulo(self, texto, nivel, capitulo=False):
        tam = {1: TIT1, 2: TIT2, 3: TIT3}[nivel]
        if capitulo:
            self.nueva()
        # el codigo en linea de un titulo va en monoespaciada, sin las comillas
        limpio = re.sub(r"`([^`]+)`", r"\1", texto)
        lineas, cur, curw = [], "", 0.0
        for palabra in limpio.split(" "):
            w = ancho_de(palabra + " ", "F2") * tam / 1000.0
            if curw + w > ANCHO and cur:
                lineas.append(cur.rstrip())
                cur, curw = "", 0.0
            cur += palabra + " "
            curw += w
        if cur.rstrip():
            lineas.append(cur.rstrip())
        for i, ln in enumerate(lineas):
            self.Ensure(tam * 1.3)
            p = self.pag()
            p.y -= tam * 1.22
            if "`" in texto:
                # se parte el titulo original por los tramos de codigo
                x = MIZQ
                for trozo in re.split(r"(`[^`]+`)", texto):
                    if not trozo:
                        continue
                    if trozo.startswith("`") and trozo.endswith("`") and len(trozo) > 2:
                        p.texto(x, p.y + tam * 0.22, trozo[1:-1], "F3", tam * 0.92)
                        x += ancho_de(trozo[1:-1], "F3") * tam * 0.92 / 1000.0
                    else:
                        p.texto(x, p.y + tam * 0.22, trozo, "F2", tam)
                        x += ancho_de(trozo, "F2") * tam / 1000.0
            else:
                p.texto(MIZQ, p.y + tam * 0.22, ln, "F2", tam)
            if i == 0 and nivel <= 2:
                p.regla(p.y - 6)
        if nivel <= 2:
            self.titulos.append((nivel, texto, len(self.paginas)))
        self.pag().y -= 8

    def vineta(self, trozos, sangria=16.0):
        self.parrafo(trozos, sangria=sangria + 12)
        p = self.pag()
        p.texto(MIZQ + sangria, p.y + INTERL - 3.5, "-", "F1", CUERPO)

    def numerada(self, n, trozos, sangria=16.0):
        self.parrafo(trozos, sangria=sangria + 18)
        p = self.pag()
        p.texto(MIZQ + sangria, p.y + INTERL - 3.5, str(n) + ".", "F2", CUERPO)

    def codigo(self, lineas):
        if not lineas:
            return
        alto = INTERL_COD * len(lineas) + 10
        # un bloque no se parte: si no cabe entero, pasa a la pagina siguiente
        if not self.sitio(alto) and self.pag().y < ALTO_PAG - MSUP:
            self.nueva()
        p = self.pag()
        p.y -= 8
        alto = INTERL_COD * len(lineas) + 8
        p.caja(MIZQ - 6, p.y - alto + 2, ANCHO + 12, alto)
        for ln in lineas:
            p.y -= INTERL_COD
            x = MIZQ
            # sangrias del fuente conservadas, como en el editor
            for trozo in re.split(r"(\t+)", ln):
                if trozo == "":
                    continue
                if trozo.startswith("\t"):
                    x += 14
                    continue
                p.texto(x, p.y + 3, trozo, "F3", COD)
                x += ancho_de(trozo, "F3") * COD / 1000.0
            if x < MIZQ + ANCHO:      # la linea no cabe entera: no la cortamos
                pass
        p.y -= 10

    def salida(self, lineas):
        """La salida esperada de un ejemplo, con su rotulo. Sin fondo: si no, se
        confunde con el codigo que tiene encima."""
        if not self.sitio(INTERL * (len(lineas) + 1) + 6):
            self.nueva()
        p = self.pag()
        p.y -= 4
        p.texto(MIZQ, p.y - INTERL, "salida", "F4", 9)
        for ln in lineas:
            p.y -= INTERL
            p.texto(MIZQ + 8, p.y + 2.5, ln or " ", "F3", COD)
        p.y -= 8

    def hueco(self, alto):
        self.pag().y -= alto

    def separador(self):
        self.Ensure(30)
        p = self.pag()
        p.y -= 15
        p.regla(p.y)
        p.y -= 15

    def admonicion(self, trozos):
        """Una caja con fondo para lo que hay que leer."""
        lineas = []
        cur, curw = [], 0.0
        maxw = ANCHO - 24
        for txt, fuente, tam in trozos:
            for palabra in re.split(r"(\s+)", txt):
                if not palabra:
                    continue
                w = ancho_de(palabra, fuente) * tam / 1000.0
                if curw + w > maxw and cur:
                    lineas.append(cur)
                    cur, curw = [], 0.0
                cur.append((palabra, fuente, tam))
                curw += w
        if cur:
            lineas.append(cur)
        alto = INTERL * len(lineas) + 16
        if not self.sitio(alto):
            self.nueva()
        p = self.pag()
        p.y -= 8
        p.caja(MIZQ - 6, p.y - alto + 4, ANCHO + 12, alto, gris=0.93)
        for ln in lineas:
            p.y -= INTERL
            x = MIZQ + 6
            for palabra, fuente, tam in ln:
                p.texto(x, p.y + tam * 0.26, palabra, fuente, tam)
                x += ancho_de(palabra, fuente) * tam / 1000.0
        p.y -= 10


# --- markdown --------------------------------------------------------------
def en_linea(texto):
    """markdown en linea -> [(texto, fuente, tamano)]"""
    out = []
    patron = re.compile(r"(`[^`]+`|\*\*[^*]+\*\*|\*[^*\s][^*]*\*)")
    for trozo in patron.split(texto):
        if not trozo:
            continue
        if len(trozo) > 2 and trozo.startswith("`") and trozo.endswith("`"):
            out.append((trozo[1:-1], "F3", CUERPO - 0.5))
        elif len(trozo) > 4 and trozo.startswith("**") and trozo.endswith("**"):
            out.append((trozo[2:-2], "F2", CUERPO))
        elif len(trozo) > 2 and trozo.startswith("*") and trozo.endswith("*"):
            out.append((trozo[1:-1], "F4", CUERPO))
        else:
            out.append((trozo, "F1", CUERPO))
    return out


def bloques(md):
    """markdown -> lista de bloques. Solo lo que este generador sabe hacer."""
    lineas = md.split("\n")
    salida = []
    i = 0
    capitulo = 0
    while i < len(lineas):
        ln = lineas[i]
        if re.match(r"^```\w*\s*$", ln):
            i += 1
            buf = []
            while i < len(lineas) and not re.match(r"^```\s*$", lineas[i]):
                buf.append(lineas[i].rstrip("\n"))
                i += 1
            i += 1
            esperado = [l[2:].strip() for l in buf if l.strip().startswith("->")]
            codigo = [l for l in buf if not l.strip().startswith("->")]
            if any(l.strip() for l in codigo):
                salida.append(("codigo", codigo))
            if esperado:
                salida.append(("salida", esperado))
            continue
        if re.match(r"^\s*>\s?", ln):
            buf = []
            while i < len(lineas) and re.match(r"^\s*>\s?", lineas[i]):
                buf.append(re.sub(r"^\s*>\s?", "", lineas[i]))
                i += 1
            salida.append(("aviso", " ".join(x.strip() for x in buf).strip()))
            continue
        if re.match(r"^\s*(---|\*\*\*|___)\s*$", ln):
            salida.append(("separador", None))
            i += 1
            continue
        m = re.match(r"^(#{1,3})\s+(.*?)\s*#*$", ln)
        if m:
            nivel = len(m.group(1))
            if nivel == 1:
                capitulo += 1
            salida.append(("titulo", (nivel, m.group(2).strip(), capitulo)))
            i += 1
            continue
        m = re.match(r"^(\s*)[-*+]\s+(.*)$", ln)
        if m:
            sangria = 14 * len(m.group(1).expandtabs(4))
            salida.append(("vineta", (sangria, en_linea(m.group(2)))))
            i += 1
            continue
        m = re.match(r"^(\s*)(\d+)\.\s+(.*)$", ln)
        if m:
            sangria = 14 * len(m.group(1).expandtabs(4))
            salida.append(("numerada", (int(m.group(2)), sangria, en_linea(m.group(3)))))
            i += 1
            continue
        if not ln.strip():
            i += 1
            continue
        buf = [ln.strip()]
        i += 1
        while i < len(lineas) and lineas[i].strip() and not re.match(
                r"^(#{1,3}\s|```|\s*>|\s*[-*+]\s|\s*\d+\.\s|\s*(---|\*\*\*|___)\s*$)", lineas[i]):
            buf.append(lineas[i].strip())
            i += 1
        salida.append(("parrafo", en_linea(" ".join(buf))))
    return salida


# --- el documento ----------------------------------------------------------
class Manual:
    """El documento y sus bloques. Recibe el Doc porque el indice y el cuerpo se
    escriben sobre el mismo: si cada uno fabricara el suyo, el indice se mediria
    sobre paginas que luego se sustituyen."""

    def __init__(self, meta, bls, doc):
        self.meta = meta
        self.bls = bls
        self.doc = doc

    def portada(self):
        d = self.doc
        p = d.nueva()
        p.y = ALTO_PAG - 210
        for ln in _partir_titulo(self.meta.get("titulo", ""), TIT1 * 1.35):
            p.texto(MIZQ, p.y, ln, "F2", TIT1 * 1.35)
            p.y -= TIT1 * 1.5
        if self.meta.get("subtitulo"):
            p.y -= 10
            for ln in _partir_titulo(self.meta["subtitulo"], 13):
                p.texto(MIZQ, p.y, ln, "F1", 13)
                p.y -= 19
        p.y -= 16
        p.ops.append("0.15 0.15 0.15 RG 1.2 w %.2f %.2f m %.2f %.2f l S"
                     % (MIZQ, p.y, MIZQ + 140, p.y))
        p.y -= 28
        for clave in ("version", "fecha", "licencia"):
            if self.meta.get(clave):
                p.texto(MIZQ, p.y, self.meta[clave], "F1", 10.5)
                p.y -= 16
        if self.meta.get("web"):
            p.texto(MIZQ, p.y, self.meta["web"], "F1", 10.5)

    def indice_con_numeros(self, numeros, titulos):
        """Va antes del cuerpo. Los titulos se sacan de los bloques y no de lo ya
        escrito, porque el indice se escribe justo antes que el cuerpo: si
        esperara a ver los titulos escritos, llegaria tarde."""
        d = self.doc
        p = d.nueva()
        p.y -= 40
        p.texto(MIZQ, p.y, "Contenido", "F2", TIT2)
        p.y -= 12
        p.regla(p.y)
        p.y -= 24
        for nivel, txt in titulos:
            sangria = 0 if nivel == 1 else 14
            tam = 10.5 if nivel == 1 else 9.8
            fuente = "F2" if nivel == 1 else "F1"
            if not d.sitio(INTERL):
                p = d.nueva()
            p.y -= INTERL
            # sin las comillas del codigo en linea: el cuerpo del documento ya no
            # las pone, y el indice las ensefia («Result y `?`»)
            p.texto(MIZQ + sangria, p.y + 3, re.sub(r"`([^`]+)`", r"\1", txt), fuente, tam)
            etiqueta = str(numeros.get(txt, ""))
            p.texto(ANCHO_PAG - MDER - ancho_de(etiqueta, "F1") * 9.8 / 1000.0,
                    p.y + 3, etiqueta, "F1", 9.8)

    def cuerpo(self):
        d = self.doc
        # el cuerpo empieza en una pagina propia: si no, el primer capitulo se
        # escribe encima de la portada o del final del indice, y los numeros de
        # pagina de la primera pasada no valen para la segunda
        if d.paginas and d.paginas[-1].ops:
            d.nueva()
        for tipo, dato in self.bls:
            if tipo == "titulo":
                nivel, texto, capitulo = dato
                d.titulo(texto, nivel, capitulo=(nivel == 1 and capitulo > 1))
            elif tipo == "parrafo":
                d.parrafo(dato)
            elif tipo == "vineta":
                sangria, trozos = dato
                d.vineta(trozos, sangria)
            elif tipo == "numerada":
                n, sangria, trozos = dato
                d.numerada(n, trozos, sangria)
            elif tipo == "codigo":
                d.codigo(dato)
            elif tipo == "salida":
                d.salida(dato)
            elif tipo == "aviso":
                d.admonicion(en_linea(dato))
            elif tipo == "separador":
                d.separador()


def _partir_titulo(texto, tam):
    lineas, cur, curw = [], "", 0.0
    for palabra in texto.split(" "):
        w = ancho_de(palabra + " ", "F2") * tam / 1000.0
        if curw + w > ANCHO and cur:
            lineas.append(cur.rstrip())
            cur, curw = "", 0.0
        cur += palabra + " "
        curw += w
    if cur.rstrip():
        lineas.append(cur.rstrip())
    return lineas


def escribir_pdf(destino, doc, meta):
    """Monta el archivo: objetos numerados correlativamente, un contenido por
    pagina comprimido, y la xref al final."""
    d = doc
    objs = {}

    def nuevo():
        n = len(objs) + 1
        objs[n] = None
        return n

    cat = nuevo()          # 1
    arbol = nuevo()        # 2
    fuente_num = {}
    for clave, base in FUENTES:
        fuente_num[clave] = nuevo()
        objs[fuente_num[clave]] = ("<< /Type /Font /Subtype /Type1 /BaseFont /%s "
                                   "/Encoding /WinAnsiEncoding >>" % base).encode("latin-1")
    nombres = " ".join("/%s %d 0 R" % (c, fuente_num[c]) for c, _ in FUENTES)

    # el pie va antes de comprimir: si no, no entra en el flujo
    for i, p in enumerate(d.paginas):
        y = MINF - 26
        if meta.get("pie"):
            p.texto(MIZQ, y, meta["pie"], "F1", 8)
        etiqueta = str(i + 1)
        p.texto(ANCHO_PAG - MDER - ancho_de(etiqueta, "F1") * 8 / 1000.0, y,
                etiqueta, "F1", 8)
        p.regla(MINF - 16, gris=0.88, grosor=0.5)

    ref_paginas = []
    ref_contenidos = []
    for i in range(len(d.paginas)):
        ref_paginas.append(nuevo())
        ref_contenidos.append(nuevo())
    info = nuevo()

    objs[cat] = ("<< /Type /Catalog /Pages %d 0 R >>" % arbol).encode("latin-1")
    objs[arbol] = ("<< /Type /Pages /Count %d /Kids [%s] >>"
                   % (len(ref_paginas),
                      " ".join("%d 0 R" % n for n in ref_paginas))).encode("latin-1")
    for i, p in enumerate(d.paginas):
        objs[ref_paginas[i]] = ("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] "
                                "/Resources << /Font << %s >> >> /Contents %d 0 R >>"
                                % (arbol, ANCHO_PAG, ALTO_PAG, nombres,
                                   ref_contenidos[i])).encode("latin-1")
        flujo = "\n".join(p.ops).encode("cp1252", "replace")
        comp = zlib.compress(flujo, 9)
        objs[ref_contenidos[i]] = (("<< /Length %d /Filter /FlateDecode >>\nstream\n"
                                    % len(comp)).encode("latin-1") + comp + b"\nendstream")
    objs[info] = ("<< /Title (%s) /Producer (hixean: tools/generar-pdf.py) "
                  "/Creator (hixean) >>"
                  % escapar(meta.get("titulo", ""))).encode("latin-1")

    total = len(objs) + 1
    salida = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for n in range(1, total):
        offsets[n] = len(salida)
        salida += ("%d 0 obj\n" % n).encode("latin-1")
        salida += objs[n]
        salida += b"\nendobj\n"
    xref = len(salida)
    salida += ("xref\n0 %d\n" % total).encode("latin-1")
    salida += b"0000000000 65535 f \n"
    for n in range(1, total):
        salida += ("%010d 00000 n \n" % offsets.get(n, 0)).encode("latin-1")
    salida += ("trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
               % (total, cat, info, xref)).encode("latin-1")
    with open(destino, "wb") as f:
        f.write(bytes(salida))
    return len(d.paginas), os.path.getsize(destino)


def main():
    if len(sys.argv) != 3:
        print("uso: generar-pdf.py <entrada.md> <salida.pdf>")
        return 2
    ruta_md, ruta_pdf = sys.argv[1], sys.argv[2]
    crudo = open(ruta_md, encoding="utf-8").read()
    meta = {}
    m = re.match(r"^---\n(.*?)\n---\n", crudo, re.S)
    if m:
        for ln in m.group(1).split("\n"):
            if ":" in ln:
                k, v = ln.split(":", 1)
                meta[k.strip()] = v.strip().strip('"')
        crudo = crudo[m.end():]
    bls = bloques(crudo)

    titulos = [(dato[0], dato[1]) for tipo, dato in bls if tipo == "titulo" and dato[0] <= 2]
    if len(set(t for _, t in titulos)) != len(titulos):
        print("FALLO: dos titulos de nivel 1 o 2 tienen el mismo nombre, y el indice "
              "solo puede senalar a uno de los dos")
        return 1

    # Pasada 1: el cuerpo sin indice delante, para saber donde cae cada titulo.
    doc = Doc(meta)
    m1 = Manual(meta, bls, doc)
    m1.portada()
    m1.cuerpo()
    base = {txt: num for _, txt, num in doc.titulos}

    # Pasada 2: cuanto ocupa el indice. Los numeros van pegados al margen derecho,
    # asi que ponerlos al final no cambia el ancho: el alto del indice es el mismo
    # con numeros que sin ellos.
    doc = Doc(meta)
    m2 = Manual(meta, bls, doc)
    m2.portada()
    m2.indice_con_numeros({}, titulos)
    paginas_indice = len(doc.paginas) - 1

    # Pasada 3: de verdad. El cuerpo va paginas_indice paginas mas tarde que en la
    # pasada 1: el indice se cuela entre la portada y el cuerpo, y en la pasada 1
    # el cuerpo ya empetaba en la pagina 2.
    numeros = {txt: base.get(txt, 1) + paginas_indice for _, txt in titulos}
    doc = Doc(meta)
    m3 = Manual(meta, bls, doc)
    m3.portada()
    m3.indice_con_numeros(numeros, titulos)
    m3.cuerpo()
    paginas, tam = escribir_pdf(ruta_pdf, doc, meta)

    # El indice no puede mentir: si un numero no corresponde a donde cae el titulo,
    # es mejor fallar aqui que publicar un indice con numeros inventados.
    for _, txt, num in doc.titulos:
        if numeros.get(txt) != num:
            print("FALLO: el indice dice pagina %s para «%s» y el titulo cae en la %d"
                  % (numeros.get(txt), txt, num))
            return 1
    print("ok     %s: %d paginas, %d bytes, indice de %d entradas"
          % (ruta_pdf, paginas, tam, len(numeros)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

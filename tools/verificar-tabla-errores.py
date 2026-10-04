#!/usr/bin/env python3
"""Vigila la tabla de diagnosticos del manual (#tabla-errores).

Cada codigo que el compilador emite tiene que tener su fila en el manual, con el
texto de los mensajes que le corresponden. Los codigos son parte de la API
(ADR 0013), asi que una tabla que se queda corta es documentacion que miente.

De uso:
    python3 tools/verificar-tabla-errores.py           # solo comprueba
    python3 tools/verificar-tabla-errores.py --add     # anade las filas que falten

--add solo inserta filas nuevas: el texto que ya esta escrito se respeta, porque
esta curado para leerse y no para salir de una regexp.
"""

import glob
import html
import os
import re
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANUAL = os.path.join(RAIZ, "docs", "manual.html")
CODIGO_RE = re.compile(r'"(E\d{4})"')
LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def _llamada_desde(txt, pos):
    """Devuelve el texto de la llamada que empieza en pos, contando parentesis."""
    nivel = 0
    en_cadena = False
    i = pos
    while i < len(txt):
        c = txt[i]
        if en_cadena:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                en_cadena = False
        elif c == '"':
            en_cadena = True
        elif c == "(":
            nivel += 1
        elif c == ")":
            nivel -= 1
            if nivel == 0:
                return txt[pos : i + 1]
        i += 1
    return txt[pos : pos + 600]


def _primer_argumento(span):
    """Lo que va justo despues del codigo, hasta la primera coma de nivel superior.

    Asi la nota de ayuda (el ultimo argumento de hx_error) no se cuela en el
    mensaje: son dos cosas distintas y en la tabla solo va el mensaje.
    """
    nivel = 0
    en_cadena = False
    i = 0
    while i < len(span):
        c = span[i]
        if en_cadena:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                en_cadena = False
        elif c == '"':
            en_cadena = True
        elif c in "([{":
            nivel += 1
        elif c in ")]}":
            nivel -= 1
        elif c == "," and nivel == 0:
            return span[:i]
        i += 1
    return span


def mensajes():
    """codigo -> lista de mensajes, tal como los escribe el compilador."""
    out = {}
    for f in sorted(glob.glob(os.path.join(RAIZ, "src", "*.c"))):
        txt = open(f, encoding="utf-8").read()
        for m in CODIGO_RE.finditer(txt):
            cod = m.group(1)
            # la llamada que contiene este codigo
            ventana = max(0, m.start() - 400)
            inicio = txt.rfind("hx_error(", ventana, m.start())
            if inicio < 0:
                inicio = txt.rfind("hx_diag_note(", ventana, m.start())
            cuerpo = ""
            if inicio >= 0:
                span = _llamada_desde(txt, inicio)
                # desde el final del literal del codigo hasta la primera coma
                off = m.start() - inicio + len(cod) + 2
                cuerpo = _primer_argumento(span[off:].lstrip(", \n\t"))
            partes = LITERAL_RE.findall(cuerpo)
            # los literales de una llamada son un solo mensaje partido por el
            # ancho de linea: se unen con espacio
            msg = " ".join(x for x in partes if x).strip()
            if not msg:
                # E0000: el codigo y el texto van en asignaciones sueltas
                sig = txt[m.start() : m.start() + 300]
                mm = re.search(r'\.msg\s*=\s*"((?:[^"\\]|\\.)*)"', sig)
                msg = mm.group(1) if mm else ""
            if msg:
                out.setdefault(cod, [])
                if msg not in out[cod]:
                    out[cod].append(msg)
    return out


def limpio(msg):
    """Los %s del formato se vuelven '…', como en el resto del manual."""
    msg = re.sub(r"%[-+ #0-9.]*[sdfclu]", "…", msg)
    msg = re.sub(r"\s+", " ", msg.replace("\n", " ")).strip()
    return msg


def html_escapado(msg, tope=110):
    msg = html.escape(limpio(msg), quote=False)
    if len(msg) > tope:
        msg = msg[:tope].rstrip() + "…"
    return msg


def fila(cod, msgs):
    partes = sorted({html_escapado(m) for m in msgs if limpio(m)})
    return '        <tr class="fila" data-codigo="%s"><td><code>%s</code></td><td>%s</td></tr>' % (
        cod,
        cod,
        " · ".join(partes),
    )


def main():
    anadir = "--add" in sys.argv
    codigos = set()
    for f in glob.glob(os.path.join(RAIZ, "src", "*.c")):
        codigos |= set(CODIGO_RE.findall(open(f, encoding="utf-8").read()))
    msgs = mensajes()
    manual = open(MANUAL, encoding="utf-8").read()
    existentes = set(re.findall(r'data-codigo="(E\d{4})"', manual))

    faltan = sorted(codigos - existentes)
    if not faltan:
        print("ok     los %d codigos de diagnostico tienen su fila en el manual" % len(codigos))
        return 0

    if not anadir:
        for cod in faltan:
            print(
                "FALLO: %s no esta en la tabla del manual (%s)"
                % (cod, " | ".join(limpio(m) for m in msgs.get(cod, []))[:110])
            )
        print("     con --add se insertan las filas que faltan")
        return 1

    m = re.search(r"(<tr><th>Código</th><th>Qué significa</th></tr>\n)", manual)
    if not m:
        print("FALLO: no se encuentra la cabecera de la tabla de errores")
        return 1
    lineas = [fila(c, msgs.get(c, [])) for c in faltan]
    manual = manual[: m.end()] + "\n".join(lineas) + "\n" + manual[m.end() :]
    open(MANUAL, "w", encoding="utf-8").write(manual)
    print("ok     anadidas %d filas a la tabla del manual: %s" % (len(lineas), ", ".join(faltan)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
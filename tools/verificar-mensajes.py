#!/usr/bin/env python3
"""Busca erratas en los mensajes de commit, que no se ven leyendo.

Un mensaje de commit se lee mal y no se nota que se lea mal. Las cinco erratas que
salieron en este repositorio eran de tres clases, y las tres se ven bien si se
mira el mensaje como bytes y no como prosa:

  - un caracter de otro alfabeto pegado dentro de una palabra. Es lo que pasa al
    teclear un `,` y que salga un ideograma chino, o un caracter coreano entre dos
    palabras en espanol: `me、成本 el tiempo` se lee «me, algo, el tiempo» y el
    algo esta ahi para siempre.
  - una mayuscula en medio de una palabra, que sale de un dedo que pisa la
    mayuscula: `rompIPIendo`, `habriaSaved`.
  - dos palabras pegadas, con el hueco de en medio: `Al修了 esto` es dos cosas,
    y `0.2.0Publication` es un numero seguido de un sustantivo.

Lo que **no** hace es exigir tildes. Los mensajes de este repositorio se escriben
casi siempre sin ellas y algunos si las llevan, asi que una puerta que las
exigiera de las dos formas solo serviria para que se apagara.

Las erratas que se corrigen se citan en el mensaje del commit que las corrige, con
el texto malo dentro. Por eso hay un modo para allowlistar esos caracteres: si no,
la puerta se quejaria de su propia correccion.

    tools/verificar-mensajes.py                # desde el primer ancestro con tag
    tools/verificar-mensajes.py --desde v0.3.0
    tools/verificar-mensajes.py --permitir "成 本 修 了 、 rompIPIendo rompIendose ..."

No va en tests/run.sh, y no por capricho. El CI clona con profundidad 1, o sea que
`git log` ve un solo commit y la puerta pasaria sin mirar nada: es la peor forma de
que una puerta falle, porque parece verde. Esta se ejecuta a mano antes de empujar, que
es cuando de verdad se puede mirar el historial.

El `--permitir` no es un silencio: es la lista de las erratas que el mensaje que las
corrige tiene que citar para decir cual eran. Sin ella, el commit que corrige una
errata se quejaria de su propia correccion.
"""
import argparse
import os
import re
import subprocess
import sys
import unicodedata

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# El patron solo casa con palabras que empiezan en minuscula y siguen con mayuscula,
# asi que de una palabra toda en mayusculas («HXC») o toda en minusculas («hxc») no
# hay que acordarse: no aparece. Solo hace falta la lista de tokens que de verdad van
# mezclados y que no son una errata.
MEZCLAS = {
    "macOS", "iOS", "querySelector", "addEventListener", "innerHTML", "textContent",
    "NaN", "ToString", "ToInt", "ToFloat", "ToUpper", "ToLower", "BigDecimal",
    "SplitMix", "Timestamp", "Hostname", "Filename", "Readonly", "Interface",
}


def otros_alfabetos(texto, permitidos):
    """Caracteres de otro alfabeto, o de control, que no sean los permitidos."""
    malos = []
    for numero, linea in enumerate(texto.split("\n"), 1):
        for c in linea:
            if c in permitidos:
                continue
            nombre = unicodedata.name(c, "")
            if ("CJK" in nombre or "HANGUL" in nombre or "HIRAGANA" in nombre
                    or "KATAKANA" in nombre or "IDEOGRAPH" in nombre
                    or unicodedata.category(c)[0] == "C"):
                malos.append((numero, c, nombre or "sin nombre"))
    return malos


def mayusculas(texto, permitidos=()):
    """Mayuscula en medio de una palabra."""
    malos = []
    for numero, linea in enumerate(texto.split("\n"), 1):
        for m in re.finditer(r"\b[a-zá-úñ]+[A-ZÁÉÍÓÚ][A-Za-zÁÉÍÓÚá-úñ]+\b", linea):
            if m.group(0) in MEZCLAS or m.group(0) in permitidos:
                continue
            malos.append((numero, m.group(0)))
    return malos


def pegadas(texto, permitidos=()):
    """Una mayuscula pegada a un numero o a un punto, sin hueco: `0.2.0Publication`."""
    malos = []
    for numero, linea in enumerate(texto.split("\n"), 1):
        for m in re.finditer(r"[0-9]\.[0-9](?:\.[0-9])?[A-ZÁÉÍÓÚ][a-zá-úñ]{2,}", linea):
            if m.group(0) in permitidos:
                continue
            malos.append((numero, m.group(0)))
    return malos


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--desde", default=None,
                    help=" ancestro desde el que revisar; por defecto, el primer tag")
    ap.add_argument("--permitir", default="",
                    help="textos exentos, separados por espacios: las erratas que el "
                         "mensaje que las corrige tiene que citar")
    args = ap.parse_args()

    desde = args.desde
    if desde is None:
        tags = subprocess.run(["git", "tag", "--sort=-v:refname"], cwd=RAIZ,
                              capture_output=True, text=True, encoding="utf-8").stdout.split()
        desde = tags[-1] if tags else None
    rango = ([desde + "^..HEAD"] if desde else ["HEAD"])

    salida = subprocess.run(["git", "log", "--format=%H%x00%B%x01"] + rango,
                            cwd=RAIZ, capture_output=True, text=True,
                            encoding="utf-8").stdout

    permitidos = set(args.permitir.split())
    fallos = 0
    for trozo in salida.split("\x01"):
        if not trozo.strip():
            continue
        partes = trozo.split("\x00", 1)
        sha = partes[0]
        mensaje = partes[1] if len(partes) > 1 else ""
        for etiqueta, lista in (("de otro alfabeto", otros_alfabetos(mensaje, permitidos)),
                               ("mayuscula en medio", mayusculas(mensaje, permitidos)),
                               ("pegadas", pegadas(mensaje, permitidos))):
            for hallazgo in lista:
                if etiqueta == "de otro alfabeto":
                    _, c, nombre = hallazgo
                    detalle = "%r (%s)" % (c, nombre)
                else:
                    _, detalle = hallazgo
                print("FALLO %s  %-20s %r  linea %d" % (sha[:9], etiqueta, detalle, hallazgo[0]))
                fallos += 1
    if fallos:
        print("\n%d errata(s) en los mensajes." % fallos)
        print("Para corregir una ya publicada hay que reescribir el commit, y eso cambia")
        print("su hash: tools/reescribir-mensajes.py lo hace comprobando que el arbol no")
        print("cambia. Quien tenga hecho fetch necesita volver con --force.")
        sys.exit(1)
    print("ok     los mensajes de commit de %s no tienen erratas de esa clase"
          % (desde if desde else "todo el historial"))
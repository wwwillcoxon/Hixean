#!/usr/bin/env python3
"""Las cifras de la pagina se comprueban contra la realidad del repositorio.

La pagina dice «22/22 pruebas», «8 896 bytes», «76 codigos de diagnostico» y
«quince ADR». Esas cuatro frases son afirmaciones sobre el proyecto, y un
proyecto que se Moreira a si mismo no puede comprobarlas a mano.

    tools/verificar-cifras.py
"""

import glob
import os
import re
import subprocess
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGINA = os.path.join(RAIZ, "site", "index.html")

fallos = []


def comprobar(condicion, mensaje):
    if not condicion:
        fallos.append(mensaje)


def pruebas_del_corpus():
    salida = subprocess.run(
        [os.path.join(RAIZ, "build", "hxc"), "test"]
        + sorted(glob.glob(os.path.join(RAIZ, "tests", "*.hxt")))
        + sorted(glob.glob(os.path.join(RAIZ, "tests", "*.hxe"))),
        capture_output=True,
        text=True,
        cwd=RAIZ,
    )
    m = re.search(r"(\d+) pruebas, (\d+) fallos", salida.stdout)
    return (int(m.group(1)), int(m.group(2))) if m else (0, -1)


def codigos_diagnostico():
    total = set()
    for fuente in glob.glob(os.path.join(RAIZ, "src", "*.c")):
        with open(fuente, encoding="utf-8") as f:
            total |= set(re.findall(r'"(E\d{4})",', f.read()))
    return len(total)


def tamano_hola_mundo():
    salida = subprocess.run(
        [os.path.join(RAIZ, "build", "hxc"), "build", "examples/hola.hxe", "-o", "build/hola"],
        capture_output=True,
        text=True,
        cwd=RAIZ,
    )
    if salida.returncode != 0:
        return 0
    return os.path.getsize(os.path.join(RAIZ, "build", "hola"))


def main():
    if not os.path.exists(PAGINA):
        print("FALLO: no esta site/index.html")
        return 1
    with open(PAGINA, encoding="utf-8") as f:
        pagina = f.read()

    cifras = {}
    for m in re.finditer(r'<b data-cuenta="(\d+)"[^>]*>([^<]*)</b><span>([^<]*)</span>', pagina):
        cifras[int(m.group(1))] = (m.group(2).strip(), m.group(3).strip())

    # 1. bytes de hola mundo
    bytes_reales = tamano_hola_mundo()
    if 8896 in cifras:
        puesto, etiqueta = cifras[8896]
        comprobar(
            re.fullmatch(r"8[\s\u00a0]896", puesto) is not None and bytes_reales == 8896,
            "la pagina dice %s bytes de hola mundo y el binario pesa %d" % (puesto, bytes_reales),
        )
    else:
        comprobar(False, "la pagina ya no muestra la cifra de bytes de hola mundo")

    # 2. pruebas del corpus
    pruebas, fallos_pruebas = pruebas_del_corpus()
    comprobar(fallos_pruebas == 0, "el corpus tiene %d fallos" % fallos_pruebas)
    if 22 in cifras:
        puesto, etiqueta = cifras[22]
        comprobar(
            re.fullmatch(r"22\s*/\s*22", puesto) is not None and pruebas == 22,
            "la pagina dice «%s» y el corpus tiene %d pruebas" % (puesto, pruebas),
        )
    else:
        comprobar(False, "la pagina ya no muestra la cifra de pruebas del corpus")

    # 3. codigos de diagnostico
    codigos = codigos_diagnostico()
    if codigos in cifras:
        puesto, etiqueta = cifras[codigos]
        comprobar(puesto == str(codigos), "la pagina dice %s codigos y hay %d" % (puesto, codigos))
        comprobar("diagn" in etiqueta, "la etiqueta de la cifra de codigos no dice que son diagnostico")
    else:
        comprobar(
            any(v[1].find("diagn") >= 0 for v in cifras.values()),
            "la pagina ya no muestra la cifra de codigos de diagnostico (hay %d)" % codigos,
        )

    # 4. numero de ADR, en la tarjeta de documentos
    adr = len(glob.glob(os.path.join(RAIZ, "docs", "adr", "*.md")))
    palabras = {"catorce": 14, "quince": 15, "dieciseis": 16, "dieciséis": 16, "catorce ": 14}
    m = re.search(r"([A-Za-zé]+) ADR", pagina)
    if m:
        dicho = palabras.get(m.group(1).lower())
        comprobar(
            dicho == adr,
            "la pagina dice «%s ADR» y hay %d" % (m.group(1), adr),
        )
    else:
        comprobar(False, "la pagina ya no dice cuantos ADR hay")

    # 5. hitos: el titulo debe cuadrar con las etiquetas M<n> del README
    with open(os.path.join(RAIZ, "README.md"), encoding="utf-8") as f:
        readme = f.read()
    hitos = sorted({int(n) for n in re.findall(r"^\| M(\d+)", readme, re.M)})
    m = re.search(r"<h2>([A-Za-zé]+) hitos", pagina)
    if m and hitos:
        dicho = palabras.get(m.group(1).lower())
        comprobar(
            dicho == max(hitos) + 1,
            "la pagina dice «%s hitos» y el README llega a M%d" % (m.group(1), max(hitos)),
        )
    else:
        comprobar(False, "la pagina ya no dice cuantos hitos hay")

    for f in fallos:
        print("FALLO:", f)
    if fallos:
        return 1
    print(
        "ok     las cifras de la pagina cuadran: %d bytes, %d/%d pruebas, %d codigos, %d ADR, %d hitos"
        % (bytes_reales, pruebas, pruebas, codigos, adr, max(hitos) + 1)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
"""Compila y ejecuta los ejemplos del documento de Hixean.

    python3 tools/verificar-guia.py docs/guia-programar.md

Cada bloque de codigo con `hixean` se compila con el compilador de este
repositorio y se ejecuta. Si el bloque lleva un comentario con la salida
esperada, se compara palabra por palabra; si no, solo se comprueba que compile y
no reviente.

Por que existe: un manual con ejemplos que no compilan es peor que no tener
manual, porque el que lo lee copia y pierde el rato. Y porque el PDF se genera
desde este markdown, un ejemplo roto se imprimiria con la misma seguridad con la
que se imprimen los que funcionan.
"""

import os
import re
import subprocess
import sys
import tempfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HXC = os.path.join(RAIZ, "build", "hxc")

RE_BLOQUE = re.compile(r"^```hixean\n(.*?)^```", re.S | re.M)


def esperado(bloque):
    """El comentario con la salida esperada va al final del bloque.

    Se escribe como una linea que empieza por `->`, que no es sintaxis de Hixean
    y asi no se confunde con codigo."""
    lineas = [l for l in bloque.split("\n") if l.strip().startswith("->")]
    return "\n".join(l.strip()[2:].strip() for l in lineas).strip()


def sin_esperado(bloque):
    return "\n".join(l for l in bloque.split("\n") if not l.strip().startswith("->"))


def compilar_y_ejecutar(bloque, tmpdir, indice):
    fuente = os.path.join(tmpdir, "ejemplo%02d.hxt" % indice)
    binario = os.path.join(tmpdir, "ejemplo%02d" % indice)
    with open(fuente, "w", encoding="utf-8") as f:
        f.write(sin_esperado(bloque))
    r = subprocess.run([HXC, "build", fuente, "-o", binario],
                       capture_output=True, text=True, cwd=RAIZ)
    if r.returncode != 0:
        return False, "no compila:\n" + r.stdout + r.stderr, None
    r = subprocess.run([binario], capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        return False, "compila pero sale con %d:\n%s" % (r.returncode, r.stderr), None
    return True, "", r.stdout.strip("\n")


def main():
    if len(sys.argv) != 2:
        print("uso: verificar-guia.py <documento.md>")
        return 2
    ruta = sys.argv[1]
    if not os.path.exists(HXC):
        print("FALLO: no esta build/hxc; ejecuta make antes")
        return 1
    with open(ruta, encoding="utf-8") as f:
        texto = f.read()
    bloques = RE_BLOQUE.findall(texto)
    if not bloques:
        print("FALLO: el documento no tiene ningun bloque ```hixean")
        return 1
    fallos = []
    con_salida = 0
    with tempfile.TemporaryDirectory() as tmp:
        for i, bloque in enumerate(bloques, 1):
            ok, motivo, salida = compilar_y_ejecutar(bloque, tmp, i)
            if not ok:
                fallos.append("ejemplo %d %s" % (i, motivo))
                continue
            quiero = esperado(bloque)
            if quiero:
                con_salida += 1
                if salida.strip() != quiero.strip():
                    fallos.append("ejemplo %d: la salida es\n---%s---\n"
                                  "y el documento dice\n---%s---" % (i, salida, quiero))
    for m in fallos:
        print("FALLO: " + m)
    if fallos:
        return 1
    print("ok     los %d ejemplos de %s compilan y dan su salida (%d con salida comprobada)"
          % (len(bloques), os.path.basename(ruta), con_salida))
    return 0


if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
"""Que las herramientas de Python hablen UTF-8 en todas partes.

Es una puerta pequena y escondida, y el fallo que se esconde es de los que no se
ven: en Windows, abrir un fichero sin decir encoding decodifica con la
codificacion del sistema, que no es UTF-8. Los documentos de Hixean son UTF-8 y
hxc escribe UTF-8, asi que un «se encontro Animal» llega con la tilde rota. Cuando
los dos lados de una comparacion estan mal decodificados se parecen y el test
pasa; en cuanto uno deja de estarlo, falla sin explicacion util.

Por eso esta puerta mira dos cosas y no una: que ningun script de tools/ abra un
fichero sin encoding, y que ninguna llamada a subprocess espere texto sin decir de
que codificacion.

    python3 tools/verificar-utf8.py
"""

import ast
import os
import re
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(RAIZ, "tools")

# Los ficheros que no son de este proyecto: no se les exige nada.
AJENOS = {"hxc_bin.py"}

SIN_ENCODING = re.compile(r"open\([^)]*\)\s*$")


def problemas_en(ruta):
    with open(ruta, encoding="utf-8") as f:
        texto = f.read()
    fallos = []
    try:
        arbol = ast.parse(texto)
    except SyntaxError as e:
        return [f"no parsea: {e}"]

    for nodo in ast.walk(arbol):
        # open(...) sin keyword encoding=
        if isinstance(nodo, ast.Call) and isinstance(nodo.func, ast.Name) \
                and nodo.func.id == "open":
            # En modo binario encoding no aplica y ademas es un error ponerlo, asi
            # que un "rb" sin encoding es lo correcto.
            # el modo puede ir como keyword o como segundo argumento posicional
            modo = ""
            if len(nodo.args) > 1 and isinstance(nodo.args[1], ast.Constant):
                modo = str(nodo.args[1].value)
            for k in nodo.keywords:
                if k.arg == "mode" and isinstance(k.value, ast.Constant):
                    modo = str(k.value.value)
            if "b" in modo.lower():
                continue
            if not any(k.arg == "encoding" for k in nodo.keywords):
                fallos.append(f"linea {nodo.lineno}: open(...) sin encoding=")
        # subprocess.run(..., text=True) sin encoding=
        if isinstance(nodo, ast.Call) and isinstance(nodo.func, ast.Attribute) \
                and nodo.func.attr == "run":
            texto_true = any(k.arg == "text" and getattr(k.value, "value", False)
                             for k in nodo.keywords)
            if texto_true and not any(k.arg == "encoding" for k in nodo.keywords):
                fallos.append(f"linea {nodo.lineno}: subprocess con text=True y sin encoding=")
    return fallos


def main():
    scripts = sorted(
        os.path.join(TOOLS, n) for n in os.listdir(TOOLS)
        if n.endswith(".py") and n not in AJENOS
    )
    fallos = 0
    for ruta in scripts:
        p = problemas_en(ruta)
        nombre = os.path.basename(ruta)
        if p:
            fallos += 1
            print(f"FALLO  {nombre}")
            for x in p:
                print(f"       {x}")
    if fallos:
        print(f"{fallos} scripts no declaran la codificacion; en Windows se leen con "
              f"la del sistema y las tildes llegan rotas")
        return 1
    print(f"ok     los {len(scripts)} scripts de tools/ dicen UTF-8 al abrir y al leer "
          f"la salida de hxc")
    return 0


if __name__ == "__main__":
    sys.exit(main())
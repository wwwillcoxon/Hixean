#!/usr/bin/env python3
"""Comprueba que los ejemplos de un documento HTML dan lo que dicen.

Cada bloque <pre><code class="lenguaje"> es un programa de Hixean completo:
se ejecuta con build/hxc y su salida se compara con el <pre> de .salida que
 tenga debajo. Si un ejemplo se rompe, el documento miente y el CI lo dice.

    tools/verificar-ejemplos.py docs/manual.html site/index.html

Los bloques que no son Hixean (un manifiesto .hxk, una sesión de terminal) se
marcan con otra clase y se saltan: en el manual es lang-hixean, en la página es
lenguaje. Con --check solo se verifican, sin ejecutar.
"""

import html
import os
import re
import subprocess
import sys
import tempfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Los documentos son UTF-8 y hxc escribe UTF-8. Decirselo a subprocess es
# obligatorio en Windows: con text=True y sin encoding se decodifica con la
# codificacion del sistema, que ahi no es UTF-8, y los diagnosticos llegan con las
# tildes rotas. El documento declara «se encontro Animal» y llegaba «se encontr�
# Animal»: los dos lados mal decodificados se parecían, hasta que uno dejo de estarlo.
sys.path.insert(0, os.path.join(RAIZ, "tools"))
from hxc_bin import HXC, existe  # en Windows el binario es build/hxc.exe

BLOQUE = re.compile(r'<(figure|pre)[^>]*>(.*?)</\1>', re.S)
CODIGO = re.compile(
    r'<pre><code class="(?:lenguaje|lang-hixean)">(.*?)</code></pre>', re.S
)
SALIDA = re.compile(r'<div class="salida"([^>]*)>.*?<pre>(.*?)</pre>', re.S)
ATRIBUTO = re.compile(r'data-([a-z-]+)="([^"]*)"')


def ejemplos(ruta):
    with open(ruta, encoding="utf-8") as f:
        texto = f.read()
    for bloque in BLOQUE.finditer(texto):
        cuerpo = bloque.group(2)
        m = CODIGO.search(cuerpo)
        if not m:
            continue
        declarado = SALIDA.search(cuerpo)
        como = "run"
        modulos = ""
        if declarado:
            for clave, valor in ATRIBUTO.findall(declarado.group(1)):
                if clave == "como":
                    como = valor
                if clave == "modulo":
                    modulos = valor
        yield {
            "linea": texto[: m.start()].count("\n") + 1,
            "codigo": html.unescape(m.group(1)),
            "salida": html.unescape(declarado.group(2)).strip() if declarado else None,
            "como": como,
            "modulos": modulos,
        }


def main(rutas, solo_check):
    if not existe():
        print("FALLO: hxc no esta compilado; no hay ni build/hxc ni build/hxc.exe (make)")
        return 1
    fallos = total = 0
    for ruta in rutas:
        for ej in ejemplos(ruta):
            total += 1
            d = tempfile.mkdtemp()
            f = os.path.join(d, "ejemplo.hxe")
            with open(f, "w", encoding="utf-8") as salida:
                salida.write(ej["codigo"])
            if ej["modulos"]:
                import shutil
                for nombre in os.listdir(os.path.join(RAIZ, ej["modulos"])):
                    origen = os.path.join(RAIZ, ej["modulos"], nombre)
                    if os.path.isfile(origen):
                        shutil.copy(origen, d)
            if ej["como"] == "check-falla":
                p = subprocess.run([HXC, "check", f], capture_output=True, encoding="utf-8", errors="replace", cwd=RAIZ)
                lineas = [l for l in p.stderr.splitlines() if "error[" in l]
                if p.returncode == 0 or not lineas:
                    fallos += 1
                    print("FALLO %s:%d: se esperaba un diagnostico y no lo hay" % (ruta, ej["linea"]))
                elif ej["salida"] not in p.stderr:
                    fallos += 1
                    print("FALLO %s:%d: el diagnostico no es el que dice el documento"
                          % (ruta, ej["linea"]))
                    print("  declarado: %r" % ej["salida"])
                    print("  obtenido:  %r" % (lineas[0] if lineas else p.stderr[:200]))
                continue
            if solo_check:
                p = subprocess.run([HXC, "check", f], capture_output=True, encoding="utf-8", errors="replace", cwd=RAIZ)
                if p.returncode != 0:
                    fallos += 1
                    print("FALLO %s:%d: hxc check no acepta el ejemplo" % (ruta, ej["linea"]))
                    print(p.stderr.strip()[:400])
                continue
            p = subprocess.run([HXC, "run", f], capture_output=True, encoding="utf-8", errors="replace", cwd=RAIZ, timeout=180)
            real = ((p.stderr if ej["como"] == "stderr" else p.stdout) or "").strip()
            if ej["salida"] is None:
                if p.returncode != 0:
                    fallos += 1
                    print("FALLO %s:%d: el ejemplo no ejecuta (rc=%d)" % (ruta, ej["linea"], p.returncode))
                    print(p.stderr.strip()[:400])
                continue
            if real != ej["salida"]:
                fallos += 1
                print("FALLO %s:%d: la salida no es la que dice el documento" % (ruta, ej["linea"]))
                print("  declarado: %r" % ej["salida"])
                print("  obtenido:  %r" % real)
                print("  stderr:    %s" % p.stderr.strip()[:300])
    if total == 0:
        print("FALLO: no se encontro ningun ejemplo que comprobar")
        return 1
    print("ok     %d ejemplos de Hixean en %s dan su salida real" % (total, ", ".join(rutas)))
    return 1 if fallos else 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--check"]
    if not args:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(args, "--check" in sys.argv[1:]))
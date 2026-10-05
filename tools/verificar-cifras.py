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
sys.path.insert(0, os.path.join(RAIZ, "tools"))
from hxc_bin import HXC, encuentra  # en Windows es build/hxc.exe
PAGINA = os.path.join(RAIZ, "site", "index.html")

fallos = []


def comprobar(condicion, mensaje):
    if not condicion:
        fallos.append(mensaje)


def pruebas_del_corpus():
    salida = subprocess.run(
        [HXC, "test"]
        + sorted(glob.glob(os.path.join(RAIZ, "tests", "*.hxt")))
        + sorted(glob.glob(os.path.join(RAIZ, "tests", "*.hxe"))),
        capture_output=True,
        encoding="utf-8", errors="replace",
        cwd=RAIZ,
    )
    m = re.search(r"(\d+) pruebas, (\d+) fallos", salida.stdout)
    return (int(m.group(1)), int(m.group(2))) if m else (0, -1)


def codigos_diagnostico():
    """Los codigos que el compilador puede emitir.

    La lista vive en tools/verificar-tabla-errores.py: asi la cifra de la pagina
    y la tabla del manual no pueden separarse. Ojo: E0000 se asigna sin coma
    detras, asi que buscar solo los que llevan coma se lo saltaba.
    """
    sys.path.insert(0, os.path.join(RAIZ, "tools"))
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "vtabla", os.path.join(RAIZ, "tools", "verificar-tabla-errores.py")
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    total = set()
    for fuente in glob.glob(os.path.join(RAIZ, "src", "*.c")):
        with open(fuente, encoding="utf-8") as f:
            total |= set(mod.CODIGO_RE.findall(f.read()))
    return len(total)


def tamano_hola_mundo():
    """Los bytes de hola mundo, que solo tienen un sentido en Linux: son los del
    perfil freestanding, y en macOS y Windows el perfil por defecto es libc y el
    binario pesa otra cosa. Por eso hay una puerta de 12 KiB solo en Linux."""
    salida = subprocess.run(
        [HXC, "build", "examples/hola.hxe", "-o", "build/hola"],
        capture_output=True,
        encoding="utf-8", errors="replace",
        cwd=RAIZ,
    )
    if salida.returncode != 0:
        return 0
    return os.path.getsize(encuentra(os.path.join(RAIZ, "build", "hola")))


def main():
    if not os.path.exists(PAGINA):
        print("FALLO: no esta site/index.html")
        return 1
    with open(PAGINA, encoding="utf-8") as f:
        pagina = f.read()

    cifras = {}
    for m in re.finditer(r'<b data-cuenta="(\d+)"[^>]*>([^<]*)</b><span>([^<]*)</span>', pagina):
        cifras[int(m.group(1))] = (m.group(2).strip(), m.group(3).strip())

    # Las cifras del sitio son de Linux por definicion: los bytes de hola mundo son
    # los del perfil freestanding, y el corpus entero con red es el que corre ahi.
    # En macOS y Windows el perfil por defecto es libc, el binario pesa otra cosa y
    # el corpus omite la prueba de sockets, asi que comparar 8896 y 34 alli seria
    # estar midiendo otra cosa. Lo que si se comprueba en todas partes es lo que no
    # depende del sistema: codigos, ADR y hitos.
    solo_linux = sys.platform.startswith("linux")

    # 1. bytes de hola mundo
    bytes_reales = tamano_hola_mundo() if solo_linux else 0
    if not solo_linux:
        comprobar(True, "")
        print("ok     los bytes de hola mundo y las pruebas del corpus se comprueban solo en Linux")
    elif 8896 in cifras:
        puesto, etiqueta = cifras[8896]
        comprobar(
            re.fullmatch(r"8[\s\u00a0]896", puesto) is not None and bytes_reales == 8896,
            "la pagina dice %s bytes de hola mundo y el binario pesa %d" % (puesto, bytes_reales),
        )
    else:
        comprobar(False, "la pagina ya no muestra la cifra de bytes de hola mundo")

    # 2. pruebas del corpus: la cifra se busca por su etiqueta, porque cambia
    if solo_linux:
        pruebas, fallos_pruebas = pruebas_del_corpus()
        comprobar(fallos_pruebas == 0, "el corpus tiene %d fallos" % fallos_pruebas)
    else:
        pruebas, fallos_pruebas = 0, 0
    de_pruebas = [(v, e) for v, e in cifras.values() if "pruebas" in e]
    if not solo_linux:
        comprobar(True, "")
    elif de_pruebas:
        puesto = de_pruebas[0][0]
        esperado = "%d/%d" % (pruebas, pruebas)
        comprobar(
            re.fullmatch(r"\s*\d+\s*/\s*\d+\s*", puesto) is not None
            and re.sub(r"\s", "", puesto) == esperado,
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
    palabras = {
        "catorce": 14,
        "quince": 15,
        "dieciseis": 16,
        "dieciséis": 16,
        "diecisiete": 17,
        "diecisiete ": 17,
        "dieciocho": 18,
        "diecinueve": 19,
        "diecinueve ": 19,
        "veinte": 20,
        "veintiuno": 21,
        "veintiún": 21,
        "veintiun ": 21,
        "veintidós": 22,
        "veintidos": 22,
        "veintitrés": 23,
        "veintitres": 23,
        "veinticuatro": 24,
        "veinticinco": 25,
        "veintiséis": 26,
        "veintiseis": 26,
    }
    m = re.search(r"([A-Za-zÀ-ÿ]+) ADR", pagina)
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
    m = re.search(r"<h2>([A-Za-zÀ-ÿ]+) hitos", pagina)
    if m and hitos:
        dicho = palabras.get(m.group(1).lower())
        comprobar(
            dicho == max(hitos) + 1,
            "la pagina dice «%s hitos» y el README llega a M%d" % (m.group(1), max(hitos)),
        )
    else:
        comprobar(False, "la pagina ya no dice cuantos hitos hay")

    # 6. la version que se anuncia, que tiene que ser la que es.
    #
    # No se busca cualquier 0.x.y: hay versiones que deben quedarse donde estan. «E0212
    # retirado en 0.2.0» es historia, y el 0.0.1 del .hxc y el 0.0.0 del .hxk son formato
    # de fichero, no la version del lenguaje. Una puerta ancha aqui daria quejidos
    # falsos y acabarian apagandola, que es peor que no tenerla.
    #
    # La version no se lee de src/common.c: se le pregunta a `hxc version`, que es la
    # unica respuesta que importa y que ademas falla si el compilador no esta construido.
    # Los trece sitios de la lista estan a proposito, y van a cambiar en cada release:
    # una puerta que hay que actualizar es una puerta que obliga a mirar.
    version = ""
    try:
        r = subprocess.run([str(HXC), "version"], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=60)
        m = re.search(r"(\d+\.\d+\.\d+)", r.stdout)
        if m:
            version = m.group(1)
    except (OSError, subprocess.SubprocessError):
        version = ""

    if solo_linux and not version:
        comprobar(False, "no se pudo leer la version de `hxc version`")
    elif version:
        # (fichero, patron) donde el patron lleva %s donde va la version
        # (fichero, que es este sitio, patron): el nombre va en el mensaje porque «no
        # anuncia la versión» sin decir cuál no lleva a ninguna parte. Un pie de pagina
        # puede ser de tres ficheros y hay cuatro sitios distintos en el mismo index.
        donde = [
            ("site/index.html", "el pie", r"Hixean %s · licencia MIT"),
            ("site/directorio.html", "el pie", r"Hixean %s · licencia MIT"),
            ("site/terminos.html", "el pie", r"Hixean %s · licencia MIT"),
            ("site/index.html", "el subtitulo de instalacion", r"guía de instalación · %s"),
            ("site/index.html", "el ejemplo de install.sh", r"sh tools/install\.sh %s"),
            ("site/index.html", "el nombre del tarball", r"hixean-%s-linux-x64\.tar\.gz"),
            ("site/index.html", "el enlace a la release", r"releases/tag/v%s"),
            ("README.md", "el ejemplo de git tag", r"git tag v%s"),
            ("packaging/homebrew/hixean.rb", "la version de la formula", r'version "%s"'),
            ("packaging/winget/hixean.yaml", "PackageVersion", r"PackageVersion: %s"),
            ("packaging/winget/hixean.yaml", "el enlace a la release", r"releases/tag/v%s"),
        ]
        for fichero, cual, patron in donde:
            ruta = os.path.join(RAIZ, fichero)
            if not os.path.exists(ruta):
                comprobar(False, "el sitio de la versión no existe: %s" % fichero)
                continue
            with open(ruta, encoding="utf-8") as f:
                texto = f.read()
            if not re.search(patron % re.escape(version), texto):
                comprobar(False, "%s, %s, no anuncia la versión %s que es la de hxc"
                          % (fichero, cual, version))

    for f in fallos:
        print("FALLO:", f)
    if fallos:
        return 1
    comun = "%d codigos, %d ADR, %d hitos" % (codigos, adr, max(hitos) + 1)
    if solo_linux:
        print("ok     las cifras de la pagina cuadran: %d bytes, %d/%d pruebas, %s"
              % (bytes_reales, pruebas, pruebas, comun))
    else:
        # nada de «0 bytes» y «0/0 pruebas»: no se han medido, y decirlo asi que
        # se midieron seria la misma mentira que este script existe para quitar
        print("ok     las cifras de la pagina cuadran (las de bytes y pruebas son "
              "de Linux): %s" % comun)
    return 0


if __name__ == "__main__":
    sys.exit(main())
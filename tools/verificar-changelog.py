#!/usr/bin/env python3
"""La sección publicada del changelog tiene que ser la que hay en su tag.

Una versión publicada es una foto: lo que se descargó del repositorio un día
concreto. Si su sección del changelog cambia después, la foto y lo que dice
de ella dejan de cuadrar, y ya no se puede saber qué se cambió de verdad ni
cuándo. Es un problema que no se ve: el fichero sigue teniendo la misma forma y
la lista de entradas sigue siendo la misma, solo hay unas líneas más o menos.

Aquí ya había pasado: 26 commits de trabajo posterior a `v0.2.0` estaban
escritos dentro del bloque de 0.2.0, así que el changelog anunciaba cosas que
nadie podía descargar, y anunciaba como ausentes cosas que sí estaban
publicadas.

La comparación no cuenta el final de línea: el tag tiene un `\r\n` suelto que
no vale la pena congelar para siempre.
"""
import os
import re
import subprocess
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHANGELOG = "CHANGELOG.md"


def published_versions():
    """Versiones con tag, de la más nueva a la más vieja."""
    try:
        out = subprocess.run(["git", "tag", "--sort=-v:refname"], cwd=RAIZ,
                             capture_output=True, text=True, encoding="utf-8",
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    return [t.strip() for t in out.splitlines() if re.match(r"^v\d+\.\d+\.\d+$", t.strip())]


def en_el_tag(tag):
    r = subprocess.run(["git", "show", "%s:%s" % (tag, CHANGELOG)], cwd=RAIZ,
                       capture_output=True, encoding="utf-8")
    if r.returncode != 0:
        return None
    return r.stdout.replace("\r\n", "\n")


def secciones(texto):
    """(título, cuerpo) de cada sección `## [...]` del fichero."""
    corte = [i for i, l in enumerate(texto.split("\n")) if l.startswith("## ")]
    lineas = texto.split("\n")
    salida = []
    for n, i in enumerate(corte):
        fin = corte[n + 1] if n + 1 < len(corte) else len(lineas)
        cuerpo = "\n".join(lineas[i:fin]).rstrip("\n")
        m = re.match(r"^## \[([^\]]+)\]", lineas[i])
        salida.append((m.group(1) if m else lineas[i], cuerpo))
    return salida


def main():
    ruta = os.path.join(RAIZ, CHANGELOG)
    with open(ruta, encoding="utf-8") as f:
        mio = f.read().replace("\r\n", "\n")
    mio_secciones = dict(secciones(mio))
    mio_orden = [t for t, _ in secciones(mio)]

    if mio_secciones and mio_orden[0].lower().startswith("unreleased"):
        print("FALLO: la primera sección es «Unreleased» pero aquí se llama "
              "«0.x.0 — sin publicar», con el número delante")
        return 1

    fallos = 0
    for tag in published_versions():
        version = tag.lstrip("v")
        theirs = en_el_tag(tag)
        if theirs is None:
            continue
        suyo = dict(secciones(theirs.replace("\r\n", "\n")))
        if version not in theirs and "[%s]" % version not in theirs:
            continue
        # la clave es lo que va entre corchetes, con el decorado de la fecha
        clave_theirs = [t for t, _ in secciones(theirs.replace("\r\n", "\n"))]
        if version not in mio_secciones:
            print("FALLO: el changelog no tiene sección para %s, que tiene un tag" % tag)
            fallos += 1
            continue
        if mio_secciones[version] != suyo.get(version, "").rstrip("\n"):
            print("FALLO: la sección [%s] no es la del tag %s" % (version, tag))
            print("       una versión publicada no se edita: lo que se cambió después "
                  "va en su propia sección, arriba")
            mios = mio_secciones[version].split("\n")
            suyos = suyo.get(version, "").rstrip("\n").split("\n")
            for k in range(max(len(mios), len(suyos))):
                a = mios[k] if k < len(mios) else "<fin>"
                b = suyos[k] if k < len(suyos) else "<fin>"
                if a != b:
                    print("       linea %d dice ahora:  %s" % (k + 1, a.strip()[:72]))
                    print("       y en el tag:        %s" % b.strip()[:72])
                    break
            fallos += 1
        else:
            print("ok     la sección [%s] es exactamente la del tag %s" % (version, tag))

    if not published_versions():
        print("ok     no hay tags de versión; nada que congelar todavía")
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
"""Reescribe el mensaje de commits concretos, sin tocar nada mas.

Un mensaje mal escrito en un commit ya publicado no se puede arreglar con un commit
nuevo encima: quedaria el original con la errata ahi para siempre, y el log
seguiria mostrandola. La unica forma de quitarla es reescribir el commit, que
cambia su hash.

Por eso este script es conservador a proposito. No usa rebase interactivo ni
`commit --amend`: reconstruye cada commit con `git commit-tree` pasandole
exactamente el arbol, el padre, el autor y el committer del original, y cambiando
solo el mensaje. Asi un commit reescrito tiene:

  - el mismo arbol, byte a byte, y se comprueba despues con `rev-parse ^{tree}`,
  - el mismo padre,
  - el mismo autor y el mismo committer, con su fecha,
  - un mensaje que es el original con unas sustituciones y nada mas.

Es decir: lo unico que cambia es el hash, y solo porque el mensaje es parte del
hash. Si dos commits tienen el mismo arbol, el mensaje era lo unico que
distinguia, asi que la comprobacion del arbol es la que demuestra que no se ha
tocado el contenido.

Se aplica sobre una rama o etiqueta que ya se ha publicado, asi que empuja con
`--force` y quien haya hecho fetch necesita volver a hacerlo. El script no
empuja: imprime lo que hizo y quien decide es quien lo lanza.
"""
import argparse
import os
import subprocess
import sys

# Commit -> sustituciones. El mensaje va sin tildes porque los dos mensajes que se
# corrigen no las tienen, y ponerlas seria destacar el arreglo en el parrafo.
CORRECCIONES = {
    "5bfb6d20": [("me、成本 el tiempo", "me costo el tiempo")],
    "61213a66": [("rompIPIendo", "rompiendo")],
    "f6bbce77": [("Al修了 esto", "Al arreglar esto"),
                 ("rompIendose", "rompiendose")],
    "a22663f2": [("habriaSaved esta hora", "habria pasado esta hora")],
}


def git(*args, entrada=None):
    r = subprocess.run(["git"] + list(args), cwd=REPO, capture_output=True,
                       input=entrada, text=True, encoding="utf-8")
    if r.returncode != 0:
        raise SystemExit("git %s fallo:\n%s" % (" ".join(args), r.stderr))
    return r.stdout


def abreviado(sha):
    """git rev-parse --short, que es el tamano que usa el usuario."""
    for n in (7, 8, 9, 10, 12, 40):
        if git("rev-parse", "--short=%d" % n, sha).strip() == sha[:n]:
            return sha[:n]
    return sha[:7]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--desde", required=True,
                    help="commit mas antiguo que hay que reescribir (incluido)")
    ap.add_argument("--rama", default="HEAD", help="ref que se mueve")
    ap.add_argument("--simular", action="store_true",
                    help="no escribe refs: solo imprime lo que haria")
    args = ap.parse_args()

    commits = git("rev-list", "--reverse", args.desde + "^..HEAD").split()
    if not commits:
        raise SystemExit("no hay commits desde %s" % args.desde)

    mapa = {}
    tocados = []
    padre = git("rev-parse", args.desde + "^").strip()

    for sha in commits:
        arbol = git("rev-parse", sha + "^{tree}").strip()
        mensaje = git("log", "-1", "--format=%B", sha)
        # %B anade un salto al final; el original tambien lo tiene
        clave = None
        for prefijo, pares in CORRECCIONES.items():
            if sha.startswith(prefijo):
                clave = prefijo
                for antes, despues in pares:
                    if antes not in mensaje:
                        raise SystemExit(
                            "%s: no aparece %r en el mensaje; el commit se ha "
                            "movido o ya estaba corregido" % (clave, antes))
                    mensaje = mensaje.replace(antes, despues)
        if clave:
            tocados.append((clave, [p[0] for p in CORRECCIONES[clave]]))

        # Aunque el mensaje no cambie hay que reconstruir el commit: si el padre se ha
        # reescrito, el hash de este cambia igual, y si no se reconstruye apuntaria a un
        # commit que ya no existe.
        env = dict(os.environ)
        for campo, fmt in (("AUTHOR", "%an"), ("AUTHOR_EMAIL", "%ae"),
                           ("AUTHOR_DATE", "%aI"), ("COMMITTER", "%cn"),
                           ("COMMITTER_EMAIL", "%ce"), ("COMMITTER_DATE", "%cI")):
            env["GIT_" + campo] = git("log", "-1", "--format=" + fmt, sha).strip()

        cmd = ["commit-tree", arbol, "-p", padre]
        nuevo_sha = subprocess.run(
            ["git"] + cmd, cwd=REPO, capture_output=True, input=mensaje,
            text=True, encoding="utf-8", env=env).stdout.strip()
        if not nuevo_sha:
            raise SystemExit("commit-tree fallo para %s" % sha)

        if git("rev-parse", nuevo_sha + "^{tree}").strip() != arbol:
            raise SystemExit("%s: el arbol cambio; abortando" % sha)
        mapa[sha] = nuevo_sha
        padre = nuevo_sha

    if not tocados:
        raise SystemExit("ningun commit tenia las erratas buscadas; no se toca nada")

    viejo = git("rev-parse", args.rama).strip()
    nuevo = padre
    print("commits reconstruidos: %d" % len(mapa))
    for clave,Charlas in sorted(tocados):
        print("  %s: %s" % (clave, "; ".join(Charlas)))
    print("  %s -> %s" % (abreviado(viejo), abreviado(nuevo)))

    if args.simular:
        print("(simulacion: no se escribe ninguna ref)")
        return 0

    git("update-ref", args.rama, nuevo, viejo)
    print("ref %s movida a %s" % (args.rama, abreviado(nuevo)))
    return 0


if __name__ == "__main__":
    REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sys.exit(main())
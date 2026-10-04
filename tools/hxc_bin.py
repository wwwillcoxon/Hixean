#!/usr/bin/env python3
"""Donde esta el compilador de verdad.

En Windows el enlazador anade .exe, asi que build/hxc es build/hxc.exe. Todo lo
que abre el binario con os.path.exists tiene que buscar los dos nombres, y hay
varios sitios: el runtime de los ejemplos, el verificador de la guia, el de las
cifras, el test de la extension y hxc size en C. Este es el unico que lo sabe.

    from hxc_bin import HXC       # la ruta que hay que ejecutar
    from hxc_bin import existe    # True si hxc esta compilado

El nombre que se ejecuta no cambia: CreateProcess y las shells de Unix anaden .exe
por su cuenta. Lo que cambia es lo que hay en disco, que es de lo que va esto.
"""
import os
import sys

_RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _con_exe(base):
    return base if base.endswith(".exe") else base + ".exe"


def encuentra(base):
    """El nombre que existe de verdad. Si no existe ninguno se devuelve el que se
    pidio, para que quien lo llame pueda imprimir algo util: asumir el .exe sin
    comprobarlo daria una ruta que no esta y un FileNotFoundError sin contexto."""
    if os.path.exists(base):
        return base
    con_exe = _con_exe(base)
    return con_exe if os.path.exists(con_exe) else base


HXC = encuentra(os.path.join(_RAIZ, "build", "hxc"))


def existe():
    """El compilador esta compilado. Sin argumento, porque casi siempre lo que se
    pregunta es justo eso."""
    return os.path.exists(HXC)

if __name__ == "__main__":
    # util desde una shell cuando una herramienta dice que falta hxc
    if existe():
        print(HXC)
        sys.exit(0)
    print("no esta ni build/hxc ni build/hxc.exe; ejecuta make", file=sys.stderr)
    sys.exit(1)
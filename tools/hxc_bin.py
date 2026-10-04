#!/usr/bin/env python3
"""Where the compiler actually is.

In Windows the linker adds .exe, so build/hxc is build/hxc.exe. Every script that
opens the binary with os.path.exists therefore has to look for both, and there are
four of them plus the C side and the vscode smoke test. This is the one place that
knows, so the others ask here instead of repeating the guess.

    from hxc_bin import HXC   # the path to run
    from hxc_bin import existe # True if it is there

The name of the runnable path does not change on Windows: CreateProcess and the
posix shell both append .exe on their own. What changes is what is on disk, which
is what this module is about.
"""
import os
import sys

_AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(_AQUI)


def _candidatos(base):
    """The names the binary can have, in the order they should be preferred."""
    yield base
    if not base.endswith(".exe"):
        yield base + ".exe"


def _base(raiz):
    return os.path.join(raiz, "build", "hxc")


def encuentra(raiz=RAIZ):
    """The first name that actually exists, or the base if none does, so callers
    can print something meaningful."""
    for nombre in _candidatos(_base(raiz)):
        if os.path.exists(nombre):
            return nombre
    return _base(raiz)


def existe(raiz=RAIZ):
    return encuentra(raiz) != _base(raiz) or os.path.exists(_base(raiz))


HXC = encuentra()

if __name__ == "__main__":
    # handy from a shell when a tool complains that hxc is missing
    print(HXC if existe() else "", end="" if existe() else "\n")
    sys.exit(0 if existe() else 1)
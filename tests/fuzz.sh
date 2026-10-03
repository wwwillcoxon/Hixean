#!/bin/sh
# Front-end a prueba de entradas rotas. El criterio no es que compile: es que
# hxc nunca muera con una señal ni se cuelgue, y que siempre diga algo.
#
# FUZZ_N=300  numero de mutaciones (por defecto 300)
# FUZZ_SEED=1 semilla; la misma semilla da la misma tanda en CI
set -e
cd "$(dirname "$0")/.."
N="${FUZZ_N:-300}"
SEED="${FUZZ_SEED:-1}"
DIR=build/fuzz
rm -rf "$DIR"
mkdir -p "$DIR/crash"

if ! command -v python3 >/dev/null 2>&1; then
  echo "ok     fuzz omitido: no hay python3"
  exit 0
fi

python3 - "$N" "$SEED" "$DIR" <<'PY'
import os, random, subprocess, sys

n, seed, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
rng = random.Random(seed)

corpus = []
for pat in ("tests/*.hxt", "tests/*.hxe", "tests/hxc/*.hxs", "tests/hxc/*.hxe",
            "examples/*.hxe", "tests/kits/*.hxk", "tests/queries/*.hxq"):
    for f in sorted(__import__("glob").glob(pat)):
        corpus.append((f, open(f, "rb").read()))
if not corpus:
    print("FALLO: no hay corpus que mutar")
    sys.exit(1)

def mutate(data, r):
    """Cada mutacion deja el archivo inservible a proposito."""
    how = r.randrange(9)
    if how == 0:                                   # truncar
        return data[: r.randrange(len(data) + 1)]
    if how == 1:                                   # voltear un byte
        i = r.randrange(len(data))
        return data[:i] + bytes([r.choice(b'"\'\\{}()[]#%$@:;,')]) + data[i + 1:]
    if how == 2:                                   # borrar una linea
        lineas = data.split(b"\n")
        if len(lineas) < 2:
            return data
        i = r.randrange(len(lineas))
        return b"\n".join(lineas[:i] + lineas[i + 1:])
    if how == 3:                                   # duplicar una linea
        lineas = data.split(b"\n")
        if not lineas:
            return data
        i = r.randrange(len(lineas))
        return b"\n".join(lineas[: i + 1] + [lineas[i]] + lineas[i + 1:])
    if how == 4:                                   # quitar todas las comillas
        return data.replace(b'"', b"")
    if how == 5:                                   # comillas sin cerrar
        return data.replace(b'"', b'" " "')
    if how == 6:                                   # ruido al final
        return data + b"\n" + bytes(r.choice(b'"\'\\{}') for _ in range(20))
    if how == 7:                                   # ruido al principio
        return bytes(r.choice(b'"\'\\{}#') for _ in range(12)) + data
    return b"\x00\x01\xff\xfe" * r.randrange(1, 8)  # bytes que no son texto

def comando(ruta):
    if ruta.endswith(".hxk"):
        return ["check", ruta]
    if ruta.endswith(".hxq"):
        return ["query", ruta, "--path", "tests/kits"]
    return ["check", ruta]

crashes = timeouts = 0
for i in range(n):
    origen, data = corpus[rng.randrange(len(corpus))]
    ext = os.path.splitext(origen)[1]
    ruta = os.path.join(out, "caso%04d%s" % (i, ext))
    with open(ruta, "wb") as f:
        f.write(mutate(data, rng))
    try:
        p = subprocess.run(["./build/hxc"] + comando(ruta), capture_output=True,
                           timeout=10)
    except subprocess.TimeoutExpired:
        timeouts += 1
        os.rename(ruta, os.path.join(out, "crash", "colgado%04d%s" % (i, ext)))
        continue
    if p.returncode < 0 or p.returncode >= 128:
        crashes += 1
        os.rename(ruta, os.path.join(out, "crash", "senal%04d%s" % (i, ext)))

print("      %d mutaciones sobre %d archivos, %d senales, %d colgados"
      % (n, len(corpus), crashes, timeouts))
if crashes or timeouts:
    print("FALLO: el front-end no sobrevive a las entradas rotas; mira build/fuzz/crash")
    sys.exit(1)
print("ok     el front-end no muere con ninguna entrada rota")
PY
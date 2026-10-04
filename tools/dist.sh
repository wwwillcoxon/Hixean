#!/bin/sh
# Empaqueta hxc para una plataforma concreta y escribe su SHA256.
#
#   tools/dist.sh <version> <plataforma>
#
# El nombre del artefacto es hixean-<version>-<plataforma>.tar.gz y el
# checksum va a dist/SHA256SUMS. La release de GitHub es la fuente de verdad:
# el resto de canales (Homebrew, winget, install.sh) solo saben leer de ahí.
set -e
cd "$(dirname "$0")/.."

VERSION="$1"
PLAT="$2"
if [ -z "$VERSION" ] || [ -z "$PLAT" ]; then
  echo "uso: tools/dist.sh <version> <plataforma>   (linux-x64, macos-arm64, windows-x64...)"
  exit 2
fi
if [ ! -x build/hxc ]; then
  echo "hxc no esta compilado: make"
  exit 1
fi

NOMBRE="hixean-$VERSION-$PLAT"
ESCENARIO="build/dist/$NOMBRE"
rm -rf "$ESCENARIO"
mkdir -p "$ESCENARIO/bin" "$ESCENARIO/docs" "$ESCENARIO/examples" \
         "$ESCENARIO/lib/hixean"

cp build/hxc "$ESCENARIO/bin/hxc"
cp LICENSE README.md CHANGELOG.md "$ESCENARIO/"
cp docs/grammar.md docs/manual.html "$ESCENARIO/docs/"
cp examples/*.hxe "$ESCENARIO/examples/"
cp tools/install.sh "$ESCENARIO/bin/install.sh"
# la biblioteca viene con el compilador: sin esto, IMPORT std.texto no funciona
# en una instalacion
cp lib/hixean/*.hxs "$ESCENARIO/lib/hixean/"

mkdir -p dist
if command -v tar >/dev/null 2>&1; then
  tar -czf "dist/$NOMBRE.tar.gz" -C build/dist "$NOMBRE"
else
  echo "no hay tar en este sistema; en Windows se usa Compress-Archive (ver release.yml)"
  exit 1
fi

if command -v sha256sum >/dev/null 2>&1; then
  SHA=$(sha256sum "dist/$NOMBRE.tar.gz" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
  SHA=$(shasum -a 256 "dist/$NOMBRE.tar.gz" | cut -d' ' -f1)
else
  SHA=""
fi
if [ -n "$SHA" ]; then
  echo "$SHA  $NOMBRE.tar.gz" >> dist/SHA256SUMS
  echo "dist/$NOMBRE.tar.gz"
  echo "sha256 $SHA"
else
  echo "dist/$NOMBRE.tar.gz (sin checksum: no hay sha256sum ni shasum)"
fi
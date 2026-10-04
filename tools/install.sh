#!/bin/sh
# Instala hxc desde la release de GitHub, verificando el checksum antes de
# tocar nada. No necesita root: deja el binario en ~/.local/bin y avisa de si
# esa carpeta está en el PATH.
#
#   curl -fsSL https://raw.githubusercontent.com/wwwillcoxon/Hixean/main/tools/install.sh | sh
#
# Si el argumento es un .tar.gz que ya existe en disco, se instala ese y no se
# descarga nada: sirve para instalar sin red y para probar el paquete antes de
# publicar una release.
#
# Variables: HIXEAN_VERSION (por defecto, la última), HIXEAN_PREFIX, HIXEAN_PLAT.
set -e

REPO="wwwillcoxon/Hixean"
API="https://api.github.com/repos/$REPO/releases/latest"

say() { printf '%s\n' "$*"; }
die() { printf 'hixean: %s\n' "$*" >&2; exit 1; }

comando() {
  if command -v "$1" >/dev/null 2>&1; then printf '%s' "$1"; return 0; fi
  for c in curl wget; do
    if command -v "$c" >/dev/null 2>&1; then printf '%s' "$c"; return 0; fi
  done
  return 1
}
descarga() {
  case "$1" in
    curl) curl -fsSL "$2" ;;
    wget) wget -qO- "$2" ;;
  esac
}
descarga_a() {
  case "$1" in
    curl) curl -fsSL -o "$3" "$2" ;;
    wget) wget -qO "$3" "$2" ;;
  esac
}

plataforma() {
  case "$(uname -s)" in
    Darwin) case "$(uname -m)" in
             arm64|aarch64) echo macos-arm64 ;;
             *) echo macos-x64 ;;
           esac ;;
    Linux) case "$(uname -m)" in
            aarch64|arm64) echo linux-arm64 ;;
            *) echo linux-x64 ;;
          esac ;;
    MINGW*|MSYS*|CYGWIN*) case "$(uname -m)" in
                          x86_64) echo windows-x64 ;;
                          *) echo windows-arm64 ;;
                        esac ;;
    *) echo "plataforma no soportada: $(uname -s)/$(uname -m)" ;;
  esac
}

[ -n "$HIXEAN_PLAT" ] || HIXEAN_PLAT=$(plataforma)
case "$HIXEAN_PLAT" in
  macos-arm64|macos-x64|linux-arm64|linux-x64) ;;
  windows-*) say "en Windows usa install.ps1 (PowerShell)"; exit 2 ;;
  *) die "plataforma no soportada: $HIXEAN_PLAT" ;;
esac

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

LOCAL=0
case "${1:-}" in
  *.tar.gz|*.tgz)
    if [ -f "$1" ]; then LOCAL=1; TAR_LOCAL="$1"; fi
    ;;
esac

if [ "$LOCAL" = 1 ]; then
  VERSION="${HIXEAN_VERSION:-$(basename "$TAR_LOCAL" | sed -n 's/^hixean-\([^-]*\)-.*/\1/p')}"
  [ -n "$VERSION" ] || VERSION="local"
  say "Hixean $VERSION (paquete local: $TAR_LOCAL)"
  cp "$TAR_LOCAL" "$TMP/hixean.tar.gz"
  tar -xzf "$TMP/hixean.tar.gz" -C "$TMP"
  PREFIX="${HIXEAN_PREFIX:-$HOME/.local}"
  mkdir -p "$PREFIX/bin"
  # en una asignacion no hay expansion de nombres de fichero (POSIX), asi que el
  # directorio se recorre con un for y no con un LIB=$TMP/hixean-*
  COPIADO=0
  for d in "$TMP"/hixean-*/; do
    [ -d "$d" ] || continue
    [ -f "$d/bin/hxc" ] || continue
    cp "$d/bin/hxc" "$PREFIX/bin/hxc"
    chmod +x "$PREFIX/bin/hxc"
    COPIADO=1
    if [ -d "$d/lib/hixean" ]; then
      mkdir -p "$PREFIX/lib/hixean"
      cp "$d"/lib/hixean/*.hxs "$PREFIX/lib/hixean/" 2>/dev/null || true
      say "biblioteca en $PREFIX/lib/hixean"
    fi
  done
  [ "$COPIADO" = 1 ] || die "el paquete no trae bin/hxc"
  say "instalado en $PREFIX/bin/hxc"
  "$PREFIX/bin/hxc" version
  exit 0
fi

CURL=$(comando curl) || die "hace falta curl o wget"

if [ -n "$HIXEAN_VERSION" ]; then
  ETIQUETA="v$HIXEAN_VERSION"
  BASE="https://github.com/$REPO/releases/download/$ETIQUETA"
else
  ETIQUETA=$(descarga "$CURL" "$API" | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
  [ -n "$ETIQUETA" ] || die "no se pudo averiguar la última release de $REPO"
  BASE="https://github.com/$REPO/releases/download/$ETIQUETA"
fi
VERSION="${ETIQUETA#v}"
say "Hixean $VERSION ($HIXEAN_PLAT)"

TAR="$TMP/hixean.tar.gz"
if ! descarga_a "$CURL" "$BASE/hixean-$VERSION-$HIXEAN_PLAT.tar.gz" "$TAR"; then
  die "no se pudo descargar $BASE/hixean-$VERSION-$HIXEAN_PLAT.tar.gz"
fi

if descarga "$CURL" "$BASE/SHA256SUMS" > "$TMP/sums" 2>/dev/null && [ -s "$TMP/sums" ]; then
  ESPERADO=$(grep " hixean-$VERSION-$HIXEAN_PLAT.tar.gz$" "$TMP/sums" | cut -d' ' -f1)
  if [ -n "$ESPERADO" ]; then
    if command -v sha256sum >/dev/null 2>&1; then OBTENIDO=$(sha256sum "$TAR" | cut -d' ' -f1)
    elif command -v shasum >/dev/null 2>&1; then OBTENIDO=$(shasum -a 256 "$TAR" | cut -d' ' -f1)
    else OBTENIDO="" ; fi
    if [ -n "$OBTENIDO" ]; then
      if [ "$OBTENIDO" != "$ESPERADO" ]; then
        rm -f "$TAR"
        die "el checksum no coincide ($OBTENIDO != $ESPERADO): descarga abandonada"
      fi
      say "checksum verificado: $OBTENIDO"
    fi
  fi
else
  say "aviso: la release no trae SHA256SUMS; no se puede verificar la descarga"
fi

tar -xzf "$TAR" -C "$TMP"
ORIGEN="$TMP/hixean-$VERSION-$HIXEAN_PLAT/bin/hxc"
[ -f "$ORIGEN" ] || die "el paquete no trae bin/hxc"

PREFIX="${HIXEAN_PREFIX:-$HOME/.local}"
mkdir -p "$PREFIX/bin"
cp "$ORIGEN" "$PREFIX/bin/hxc"
chmod +x "$PREFIX/bin/hxc"
[ -f "$TMP/hixean-$VERSION-$HIXEAN_PLAT/bin/install.sh" ] &&
  cp "$TMP/hixean-$VERSION-$HIXEAN_PLAT/bin/install.sh" "$PREFIX/bin/hixean-install.sh"

# La biblioteca va a <prefijo>/lib/hixean, que es donde hxc la mira: esta en
# bin/lib si se copia al lado del binario.
BIBLIO="$TMP/hixean-$VERSION-$HIXEAN_PLAT/lib/hixean"
if [ -d "$BIBLIO" ]; then
  mkdir -p "$PREFIX/lib/hixean"
  cp "$BIBLIO"/*.hxs "$PREFIX/lib/hixean/" 2>/dev/null || true
  say "biblioteca en $PREFIX/lib/hixean"
fi

say "instalado en $PREFIX/bin/hxc"
"$PREFIX/bin/hxc" version
case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) say ""; say "ese directorio no está en tu PATH. Añádelo:"; say "  export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac
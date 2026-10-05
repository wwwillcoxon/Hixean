# Formula de Homebrew. Se publica en un taps propio; mientras tanto:
#
#   brew tap wwwillcoxon/hixean https://github.com/wwwillcoxon/homebrew-hixean
#   brew install hixean
#
# La release de GitHub es la fuente de verdad: aquí solo se declara la URL y
# el sha256 que publica tools/dist.sh.
#
# 0.3.0 publica un artefacto, el de linux-x64. Las otras tres casillas se
# esconden porque Homebrew exige una URL que exista para cada arquitectura: una
# `brew install` que descarga un 404 no es una instalacion, es una promesa rota.
# Se abren cuando su plataforma entre en la matriz de la release con la puerta
# verde (ver el comentario de .github/workflows/release.yml, que dice cual es lo
# que falta en cada una).
class Hixean < Formula
  desc "Lenguaje AOT que compila a C11, sin VM y sin unwinding"
  homepage "https://github.com/wwwillcoxon/Hixean"
  version "0.3.0"
  license "MIT"

  on_macos do
    # Sin artefacto para macOS todavia: el perfil por defecto es libc y el
    # compilador construye, pero falla una prueba del corpus (la interpolacion de
    # un entero en una funcion generica). Sin el mismo no hay binario que publicar.
    odie "sin artefacto para macOS en la 0.3.0: la release solo trae linux-x64"
  end

  on_linux do
    if Hardware::CPU.arm?
      # arm64 necesita el compilador cruzado en la puerta de la release; el camino
      # es identico a x64 en cuanto exista.
      odie "sin artefacto para linux-arm64 todavia: falta el compilador cruzado"
    else
      # sha256 copiado de SHA256SUMS de la release v0.3.0, y comprobado bajando el
      # tarball aparte y pasarle `sha256sum -c`: copiarlo sin comprobar seria una
      # cadena de fe. La release publica un solo artefacto, el de linux-x64; las otras
      # tres casillas se esconden porque Homebrew exige una URL que exista para cada
      # arquitectura.
      url "https://github.com/wwwillcoxon/Hixean/releases/download/v0.3.0/hixean-0.3.0-linux-x64.tar.gz"
      sha256 "5d109e86d40f08f0deb8034e7f52f889c3b388d77ddb7b1cedfbf3e9b9354639"
    end
  end

  def install
    bin.install "bin/hxc"
    doc.install "README.md", "CHANGELOG.md" if buildpath.directory?("README.md")
    doc.install "docs/grammar.md", "docs/manual.html" if buildpath.directory?("docs")
    pkg.install "examples"
  end

  test do
    assert_match "hxc #{version}", shell_output("#{bin}/hxc version")
  end
end

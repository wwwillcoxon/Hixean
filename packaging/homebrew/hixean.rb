# Formula de Homebrew. Se publica en un taps propio; mientras tanto:
#
#   brew tap wwwillcoxon/hixean https://github.com/wwwillcoxon/homebrew-hixean
#   brew install hixean
#
# La release de GitHub es la fuente de verdad: aquí solo se declara la URL y
# el sha256 que publica tools/dist.sh.
class Hixean < Formula
  desc "Lenguaje AOT que compila a C11, sin VM y sin unwinding"
  homepage "https://github.com/wwwillcoxon/Hixean"
  version "0.1.0"
  license "MIT"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/wwwillcoxon/Hixean/releases/download/v0.1.0/hixean-0.1.0-macos-arm64.tar.gz"
      sha256 "REEMPLAZAR_CON_EL_SHA256_DE_macos-arm64"
    else
      url "https://github.com/wwwillcoxon/Hixean/releases/download/v0.1.0/hixean-0.1.0-macos-x64.tar.gz"
      sha256 "REEMPLAZAR_CON_EL_SHA256_DE_macos-x64"
    end
  end

  on_linux do
    if Hardware::CPU.arm?
      url "https://github.com/wwwillcoxon/Hixean/releases/download/v0.1.0/hixean-0.1.0-linux-arm64.tar.gz"
      sha256 "REEMPLAZAR_CON_EL_SHA256_DE_linux-arm64"
    else
      url "https://github.com/wwwillcoxon/Hixean/releases/download/v0.1.0/hixean-0.1.0-linux-x64.tar.gz"
      sha256 "REEMPLAZAR_CON_EL_SHA256_DE_linux-x64"
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

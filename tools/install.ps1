# Instala hxc desde la release de GitHub en Windows, verificando el checksum.
#
#   powershell -ExecutionPolicy Bypass -File tools\install.ps1
#
# Parametros: -Version, -Prefix (por defecto $env:LOCALAPPDATA\Programs\hixean)

$ErrorActionPreference = "Stop"

$Repo = "wwwillcoxon/Hixean"

function Say($m) { Write-Host $m }
function Die($m) { Write-Error "hixean: $m"; exit 1 }

if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") {
    $Plat = "windows-arm64"
} else {
    $Plat = "windows-x64"
}

if (-not $Version) {
    $release = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest"
    $Version = $release.tag_name.TrimStart("v")
}
if (-not $Prefix) {
    $Prefix = Join-Path $env:LOCALAPPDATA "Programs\hixean"
}

$base = "https://github.com/$Repo/releases/download/v$Version"
$tarball = "hixean-$Version-$Plat.tar.gz"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("hixean-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null

Say "Hixean $Version ($Plat)"
$archive = Join-Path $tmp $tarball
Invoke-WebRequest "$base/$tarball" -OutFile $archive

$sumsUrl = "$base/SHA256SUMS"
try {
    $sums = (Invoke-WebRequest $sumsUrl).Content
    $line = ($sums -split "`n") | Where-Object { $_ -match " $tarball$" } | Select-Object -First 1
    if ($line) {
        $esperado = $line.Split(" ")[0].Trim()
        $obtenido = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
        if ($obtenido -ne $esperado) {
            Remove-Item $archive -Force
            Die "el checksum no coincide ($obtenido != $esperado): descarga abandonada"
        }
        Say "checksum verificado: $obtenido"
    }
} catch {
    Say "aviso: la release no trae SHA256SUMS; no se puede verificar la descarga"
}

# Windows no trae tar en PATH en toda instalacion; si esta, se usa; si no, el zip
$bin = Join-Path $tmp "hixean-$Version-$Plat\bin\hxc.exe"
if (-not (Test-Path $bin)) {
    if (Get-Command tar -ErrorAction SilentlyContinue) {
        tar -xzf $archive -C $tmp
    } else {
        $zip = "hixean-$Version-$Plat.zip"
        Invoke-WebRequest "$base/$zip" -OutFile (Join-Path $tmp $zip)
        Expand-Archive (Join-Path $tmp $zip) -DestinationPath $tmp
        $bin = Join-Path $tmp "hixean-$Version-$Plat\bin\hxc.exe"
    }
}
if (-not (Test-Path $bin)) { Die "el paquete no trae bin\hxc.exe" }

New-Item -ItemType Directory -Force -Path (Join-Path $Prefix "bin") | Out-Null
Copy-Item $bin (Join-Path $Prefix "bin\hxc.exe") -Force

Say "instalado en $Prefix\bin\hxc.exe"
& (Join-Path $Prefix "bin\hxc.exe") version
Say "si esa carpeta no está en el PATH, añádela con:"
Say "  [Environment]::SetEnvironmentVariable('Path', [Environment]::GetEnvironmentVariable('Path','User') + ';$Prefix\bin', 'User')"
Remove-Item $tmp -Recurse -Force
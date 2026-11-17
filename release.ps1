# Sube la versión actual de iw7s a GitHub y crea la release con iw7s.exe, png2dds.exe y la carpeta iw7s.
#
# Uso (desde la carpeta de iw7-mod, después de compilar con build.ps1):
#   powershell -ExecutionPolicy Bypass -File .\release.ps1
#   powershell -ExecutionPolicy Bypass -File .\release.ps1 -NoGit       # no hace commit/push, solo el zip y la release
#   powershell -ExecutionPolicy Bypass -File .\release.ps1 -NoRelease   # solo commit/push y el zip
#
# La revisión sale de revision.txt (p. ej. 1049b -> tag r1049b). Las notas salen de la sección de esa revisión en
# CHANGES.md. Si GitHub CLI (gh) está instalado y con sesión iniciada crea la release él solo; si no, abre la página de
# nueva release en el navegador con las notas copiadas al portapapeles y el zip seleccionado en el Explorador.

param(
    [switch]$NoGit,
    [switch]$NoRelease,
    [string]$Repo = "joseupp/iw7s"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root

$revision = "r" + (Get-Content (Join-Path $root "revision.txt") -Raw).Trim()
$exe = Join-Path $root "build\bin\x64\Release\iw7s.exe"
$png2dds = Join-Path $root "build\bin\x64\Release\png2dds.exe"
$data = Join-Path $root "data\iw7s"

# El exe tiene que ser de esta revisión (lleva la cadena rXXXXb dentro)
if (-not (Test-Path $exe)) { throw "No existe $exe. Compila primero con build.ps1" }
$exeText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe))
if (-not $exeText.Contains($revision)) {
    Write-Host "iw7s.exe no es de $revision (revision.txt). Compila primero con build.ps1" -ForegroundColor Red
    exit 1
}

# --- Git: commit y push ---
if (-not $NoGit) {
    git fetch origin
    if ($LASTEXITCODE -ne 0) { throw "git fetch ha fallado" }

    # Si en GitHub hay commits que no están aquí (p. ej. ediciones del README en la web), se pone HEAD encima de
    # origin/main sin tocar los archivos de la carpeta
    git merge-base --is-ancestor HEAD origin/main
    if ($LASTEXITCODE -eq 0) {
        git reset --mixed -q origin/main
    }
    else {
        git pull --rebase --autostash origin main
        if ($LASTEXITCODE -ne 0) { throw "git pull ha fallado: resuelve los conflictos y vuelve a ejecutar" }
    }

    git add -A
    $status = git status --short
    if ($status) {
        Write-Host "Cambios para $revision`:" -ForegroundColor Cyan
        $status | ForEach-Object { Write-Host "  $_" }
        $answer = Read-Host "Hacer commit y push? (s/n)"
        if ($answer -ne "s") { Write-Host "Cancelado" -ForegroundColor Yellow; exit 1 }

        git commit -q -m "$revision"
        if ($LASTEXITCODE -ne 0) { throw "git commit ha fallado" }
    }
    else {
        Write-Host "No hay cambios que subir" -ForegroundColor Yellow
    }

    git push origin main
    if ($LASTEXITCODE -ne 0) { throw "git push ha fallado" }
    Write-Host "Subido a GitHub" -ForegroundColor Green
}

# --- Zip de la release: iw7s.exe, png2dds.exe y la carpeta iw7s (data\iw7s) ---
$releaseDir = Join-Path $root "build\release"
$stage = Join-Path $releaseDir $revision
$zip = Join-Path $releaseDir "iw7s-$revision.zip"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $zip) { Remove-Item $zip -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item $exe $stage
if (Test-Path $png2dds) { Copy-Item $png2dds $stage }
Copy-Item $data (Join-Path $stage "iw7s") -Recurse

Write-Host "Creando $zip..." -ForegroundColor Cyan
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force
Write-Host "Zip: $zip" -ForegroundColor Green

if ($NoRelease) { exit 0 }

# --- Notas: sección de esta revisión en CHANGES.md + instalación ---
$changes = Get-Content (Join-Path $root "CHANGES.md") -Encoding UTF8
$section = @()
$inSection = $false
foreach ($line in $changes) {
    if ($line -match "^## ") {
        if ($inSection) { break }
        if ($line -match "^## $revision\b") { $inSection = $true; continue }
    }
    elseif ($inSection) { $section += $line }
}

$notes = @"
## Installation

Required files:
``````
├── iw7s.exe
├── png2dds.exe   (optional, converts PNG textures to the game's DDS format)
└── iw7s/
    └── (useful files)
``````

Extract the zip and move all files to your Call of Duty®: Infinite Warfare game folder.

The first time, open Steam with your account and run ``iw7s.exe``. After that, Steam doesn't need to be open.

> You must own Call of Duty®: Infinite Warfare on Steam. Pirated copies are not supported.

## Cambios

$(($section -join "`n").Trim())
"@
$notesFile = Join-Path $releaseDir "notes-$revision.md"
[IO.File]::WriteAllText($notesFile, $notes, (New-Object Text.UTF8Encoding($false)))

$title = "IW7S $revision"
$gh = Get-Command gh -ErrorAction SilentlyContinue
if ($gh) {
    gh auth status 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Creando la release $revision en GitHub..." -ForegroundColor Cyan
        gh release create $revision $zip --repo $Repo --target main --title $title --notes-file $notesFile
        if ($LASTEXITCODE -ne 0) { throw "gh release create ha fallado" }
        Write-Host "Release creada: https://github.com/$Repo/releases/tag/$revision" -ForegroundColor Green
        exit 0
    }
}

# Sin gh: página de nueva release + notas en el portapapeles + zip en el Explorador
Set-Clipboard -Value $notes
Start-Process "https://github.com/$Repo/releases/new?tag=$revision&target=main&title=$([uri]::EscapeDataString($title))"
Start-Process explorer.exe "/select,`"$zip`""
Write-Host "GitHub CLI (gh) no esta disponible o sin sesion: se ha abierto la pagina de nueva release." -ForegroundColor Yellow
Write-Host "Las notas estan en el portapapeles (pegalas en la descripcion) y arrastra el zip a la release." -ForegroundColor Yellow

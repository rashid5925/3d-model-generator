# shapeforge installer for Windows.
#   irm https://raw.githubusercontent.com/rashid5925/3d-model-generator/main/scripts/install.ps1 | iex
# Options (environment variables): SHAPEFORGE_VERSION, SHAPEFORGE_INSTALL_DIR, SHAPEFORGE_CLIENT
$ErrorActionPreference = "Stop"

$Repo = if ($env:SHAPEFORGE_REPO) { $env:SHAPEFORGE_REPO } else { "rashid5925/3d-model-generator" }
$Version = if ($env:SHAPEFORGE_VERSION) { $env:SHAPEFORGE_VERSION } else { "latest" }
$InstallDir = if ($env:SHAPEFORGE_INSTALL_DIR) { $env:SHAPEFORGE_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA "Programs\shapeforge" }
$Asset = "shapeforge-windows-x64.exe"
$Base = if ($Version -eq "latest") { "https://github.com/$Repo/releases/latest/download" } else { "https://github.com/$Repo/releases/download/$Version" }

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$Tmp = New-Item -ItemType Directory -Force -Path (Join-Path ([IO.Path]::GetTempPath()) ("shapeforge-" + [Guid]::NewGuid()))
try {
    Write-Host "Downloading $Asset ($Version)..."
    Invoke-WebRequest -UseBasicParsing "$Base/$Asset" -OutFile (Join-Path $Tmp "shapeforge.exe")
    Invoke-WebRequest -UseBasicParsing "$Base/SHA256SUMS" -OutFile (Join-Path $Tmp "SHA256SUMS")

    $Line = Get-Content (Join-Path $Tmp "SHA256SUMS") | Where-Object { $_ -match " $([regex]::Escape($Asset))$" }
    if (-not $Line) { throw "no checksum for $Asset" }
    $Expected = ($Line -split "\s+")[0].ToLower()
    $Actual = (Get-FileHash (Join-Path $Tmp "shapeforge.exe") -Algorithm SHA256).Hash.ToLower()
    if ($Expected -ne $Actual) { throw "checksum mismatch for $Asset" }

    $Exe = Join-Path $InstallDir "shapeforge.exe"
    Move-Item -Force (Join-Path $Tmp "shapeforge.exe") $Exe
} finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}

$UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (-not ($UserPath -split ";" | Where-Object { $_ -eq $InstallDir })) {
    [Environment]::SetEnvironmentVariable("Path", "$UserPath;$InstallDir", "User")
    Write-Host "Added $InstallDir to your PATH (open a new terminal to use 'shapeforge')."
}
Write-Host "Installed $Exe ($(& $Exe version))"

if ($env:SHAPEFORGE_CLIENT) { & $Exe install $env:SHAPEFORGE_CLIENT }

Write-Host ""
Write-Host "Next: connect it to your AI app, e.g."
Write-Host "  shapeforge install claude-code"
Write-Host "Agents: claude-code, claude-desktop, cursor, windsurf, vscode, copilot-cli, opencode, codex, all"
Write-Host "Then ask: `"Model a coffee mug and export it as OBJ.`""

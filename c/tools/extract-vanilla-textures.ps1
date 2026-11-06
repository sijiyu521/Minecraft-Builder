#requires -Version 5
<#
.SYNOPSIS
    Extract the vanilla Minecraft block textures into c/textures/block.

.DESCRIPTION
    Minecraft's textures belong to Mojang and are not shipped with this
    repository. This helper copies the 16x16 block PNGs out of your own
    installation into the folder the previewer auto-detects.

    Source can be:
      - omitted: the newest version jar found under %APPDATA%\.minecraft\versions
      - a version jar / any client jar (zip)
      - an extracted resource pack folder

    The output goes to <repo>\c\textures\block and is git-ignored.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File c\tools\extract-vanilla-textures.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File c\tools\extract-vanilla-textures.ps1 `
        -Source "$env:APPDATA\.minecraft\resourcepacks\MyPack"
#>
param(
    [string]$Source,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$cDir = Split-Path -Parent $PSScriptRoot          # <repo>\c
if (-not $OutDir) { $OutDir = Join-Path $cDir 'textures\block' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

function Write-BlockPng([byte[]]$bytes, [string]$name) {
    [System.IO.File]::WriteAllBytes((Join-Path $OutDir $name), $bytes)
}

$written = 0
$prefix = 'assets/minecraft/textures/block/'

if (-not $Source) {
    $versions = Join-Path $env:APPDATA '.minecraft\versions'
    if (Test-Path $versions) {
        $jars = Get-ChildItem $versions -Directory |
            ForEach-Object { Get-ChildItem $_.FullName -Filter '*.jar' -File -ErrorAction SilentlyContinue } |
            Sort-Object LastWriteTime -Descending
        if ($jars) { $Source = $jars[0].FullName }
    }
}

if (-not $Source) {
    Write-Error @"
No Minecraft client jar found.
Pass -Source with your .minecraft\versions\<version>\<version>.jar,
or with an extracted resource pack folder.
"@
    exit 1
}

Write-Host "Source: $Source"
Write-Host "Output: $OutDir"

if (Test-Path $Source -PathType Container) {
    $blockDir = Join-Path $Source 'assets\minecraft\textures\block'
    if (-not (Test-Path $blockDir)) {
        Write-Error "Not a resource pack root (missing assets\minecraft\textures\block): $Source"
        exit 1
    }
    Get-ChildItem $blockDir -Filter '*.png' -File | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $OutDir $_.Name) -Force
        $written++
    }
} else {
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Source)
    try {
        foreach ($entry in $zip.Entries) {
            if (-not $entry.FullName.StartsWith($prefix, 'OrdinalIgnoreCase')) { continue }
            if (-not $entry.FullName.EndsWith('.png', 'OrdinalIgnoreCase')) { continue }
            if ($entry.FullName.Length -le $prefix.Length) { continue }
            $name = $entry.Name
            if ($entry.FullName.Contains('/') -and ($entry.FullName.Substring($prefix.Length).Contains('/'))) {
                continue  # skip subfolders like block/foo/bar.png
            }
            $stream = $entry.Open()
            $ms = New-Object System.IO.MemoryStream
            $stream.CopyTo($ms)
            $stream.Dispose()
            Write-BlockPng $ms.ToArray() $name
            $ms.Dispose()
            $written++
        }
    } finally {
        $zip.Dispose()
    }
}

Write-Host "Extracted $written block textures."
if ($written -eq 0) {
    Write-Error 'No block textures found in that source.'
    exit 1
}
Write-Host 'The app auto-detects c\textures\block; you can also pass --textures <dir>.'

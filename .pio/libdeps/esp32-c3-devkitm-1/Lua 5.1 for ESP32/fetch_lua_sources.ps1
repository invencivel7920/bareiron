# Fetches Lua 5.1.5 source and copies it into src/ for the lua511-esp32 Arduino library.
# Run from the library folder: .\fetch_lua_sources.ps1

$ErrorActionPreference = "Stop"
$LibRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir = Join-Path $LibRoot "src"
$TempDir = Join-Path $env:TEMP "lua-5.1.5-fetch"

# Filer vi IKKE skal ha (standalone tolker/kompilator, Windows-spesifikke, ekstra LuaDist)
$Exclude = @("lua.c", "luac.c", "print.c", "wmain.c", "loadlib_rel.c")

function Get-LuaSource {
    # Prøv LuaDist GitHub (zip, enkel å pakke ut på Windows)
    $zipUrl = "https://github.com/LuaDist/lua/archive/refs/tags/5.1.5.zip"
    $zipPath = Join-Path $env:TEMP "lua-5.1.5.zip"

    Write-Host "Downloading Lua 5.1.5 from LuaDist..."
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath -UseBasicParsing
    } catch {
        Write-Host "Download failed. Try manual setup:"
        Write-Host "  1. Go to https://github.com/LuaDist/lua/releases/tag/5.1.5"
        Write-Host "  2. Download Source code (zip)"
        Write-Host "  3. Extract and copy all .c and .h from the archive's src/ folder into $SrcDir"
        Write-Host "     Exclude: lua.c, luac.c, print.c, wmain.c, loadlib_rel.c"
        exit 1
    }

    if (Test-Path $TempDir) { Remove-Item -Recurse -Force $TempDir }
    New-Item -ItemType Directory -Path $TempDir | Out-Null
    Expand-Archive -Path $zipPath -DestinationPath $TempDir
    $extracted = Get-ChildItem $TempDir -Directory | Select-Object -First 1
    if (-not $extracted) { throw "Uventet zip-innhold" }
    return $extracted.FullName
}

function Copy-LuaFiles {
    param([string]$LuaDir)

    New-Item -ItemType Directory -Force -Path $SrcDir | Out-Null

    $copied = 0
    Get-ChildItem -Path $LuaDir -Filter *.c | ForEach-Object {
        if ($_.Name -notin $Exclude) {
            Copy-Item $_.FullName -Destination $SrcDir -Force
            $copied++
            Write-Host "  $($_.Name)"
        }
    }
    Get-ChildItem -Path $LuaDir -Filter *.h | ForEach-Object {
        Copy-Item $_.FullName -Destination $SrcDir -Force
        $copied++
        Write-Host "  $($_.Name)"
    }

    return $copied
}

# Main
Write-Host "lua511-esp32: fetching Lua 5.1.5 into src/..."
$luaRoot = Get-LuaSource
$srcSub = Join-Path $luaRoot "src"
if (Test-Path $srcSub) { $luaDir = $srcSub } else { $luaDir = $luaRoot }

New-Item -ItemType Directory -Force -Path $SrcDir | Out-Null
# luaconf.h is in the archive root, not in src/
$luaconf = Join-Path $luaRoot "luaconf.h"
if (Test-Path $luaconf) {
    Copy-Item $luaconf -Destination $SrcDir -Force
    Write-Host "  luaconf.h (from archive root)"
}

$n = Copy-LuaFiles -LuaDir $luaDir
if (Test-Path $luaconf) { $n++ }
Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:TEMP "lua-5.1.5.zip") -ErrorAction SilentlyContinue
Write-Host "Done. $n files copied to $SrcDir"
Write-Host "In Arduino IDE: Sketch -> Include Library -> Add .ZIP Library and select the lua511-esp32 folder (or copy it to Arduino/libraries/)."

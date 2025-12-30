#!/usr/bin/env pwsh
#
# KiCad Development Build and Run Script for Windows
# ==================================================
# Usage:
#   .\dev-build.ps1              # Build KiCad (if needed) and run it
#   .\dev-build.ps1 build        # Force a full rebuild
#   .\dev-build.ps1 run          # Just run (skip build)
#   .\dev-build.ps1 clean        # Clean build directory
#   .\dev-build.ps1 configure    # Reconfigure CMake
#   .\dev-build.ps1 -j 8         # Build with 8 parallel jobs
#   .\dev-build.ps1 -j8          # Build with 8 parallel jobs
#   .\dev-build.ps1 --build-preset win64-release
#   .\dev-build.ps1 --configure-preset msvc-win64-release

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Info([string]$Message) { Write-Host "[info]  $Message" -ForegroundColor Cyan }
function Warn([string]$Message) { Write-Host "[warn]  $Message" -ForegroundColor Yellow }
function Err([string]$Message)  { Write-Host "[error] $Message" -ForegroundColor Red }
function Ok([string]$Message)   { Write-Host "[ok]    $Message" -ForegroundColor Green }

$RepoRoot = Split-Path -Parent $PSCommandPath

function Import-DotEnv([string]$Path) {
    foreach ($line in Get-Content -Path $Path) {
        $t = $line.Trim()
        if (-not $t) { continue }
        if ($t.StartsWith("#")) { continue }

        $eq = $t.IndexOf("=")
        if ($eq -lt 1) { continue }

        $key = $t.Substring(0, $eq).Trim()
        $val = $t.Substring($eq + 1).Trim()

        # Strip optional wrapping quotes: KEY="value" or KEY='value'
        if ($val.Length -ge 2) {
            $first = $val[0]
            $last = $val[$val.Length - 1]
            if (($first -eq '"' -and $last -eq '"') -or ($first -eq "'" -and $last -eq "'")) {
                $val = $val.Substring(1, $val.Length - 2)
            }
        }

        if ($key) {
            Set-Item -Path ("Env:" + $key) -Value $val
        }
    }
}

# Load repo-root .env (.env convenience)
Import-DotEnv (Join-Path $RepoRoot ".env")
$Action = "build_and_run"
$Jobs = $null
$BuildPreset = "win64-debug"
$ConfigurePreset = "msvc-win64-debug"
$AgentPanelDev = $false
$AgentPanelDevUrl = $null

for ($i = 0; $i -lt $args.Count; $i++) {
    $arg = $args[$i]
    switch -Regex ($arg) {
        "^build$" { $Action = "build"; continue }
        "^run$" { $Action = "run"; continue }
        "^clean$" { $Action = "clean"; continue }
        "^configure$" { $Action = "configure"; continue }
        "^-j\d+$" { $Jobs = [int]$arg.Substring(2); continue }
        "^-j$" {
            if ($i + 1 -ge $args.Count) { Err "Missing value after -j"; exit 1 }
            $Jobs = [int]$args[$i + 1]
            $i++
            continue
        }
        "^--build-preset$" {
            if ($i + 1 -ge $args.Count) { Err "Missing value after --build-preset"; exit 1 }
            $BuildPreset = $args[$i + 1]
            $i++
            continue
        }
        "^--configure-preset$" {
            if ($i + 1 -ge $args.Count) { Err "Missing value after --configure-preset"; exit 1 }
            $ConfigurePreset = $args[$i + 1]
            $i++
            continue
        }
        "^--agent-panel-dev$" { $AgentPanelDev = $true; continue }
        "^--agent-panel-dev-url$" {
            if ($i + 1 -ge $args.Count) { Err "Missing value after --agent-panel-dev-url"; exit 1 }
            $AgentPanelDevUrl = $args[$i + 1]
            $i++
            continue
        }
        default { Err "Unknown option: $arg"; exit 1 }
    }
}

Info "Repository root: $RepoRoot"
Info "Configure preset: $ConfigurePreset"
Info "Build preset: $BuildPreset"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Err "CMake is not installed or not on PATH."
    exit 1
}

$PresetsPath = Join-Path $RepoRoot "CMakePresets.json"
$PresetsSamplePath = Join-Path $RepoRoot "CMakePresets.json.sample"

if (-not (Test-Path $PresetsPath)) {
    Err "CMakePresets.json not found."
    if (Test-Path $PresetsSamplePath) {
        Warn "Copy CMakePresets.json.sample to CMakePresets.json and set VCPKG_ROOT."
    }
    exit 1
}

function Resolve-BuildDir([string]$PresetName) {
    $presetJson = Get-Content -Raw -Path $PresetsPath | ConvertFrom-Json
    $preset = $presetJson.configurePresets | Where-Object { $_.name -eq $PresetName } | Select-Object -First 1
    if (-not $preset) {
        return (Join-Path $RepoRoot ("build\" + $PresetName))
    }
    $binaryDir = $null
    if ($preset.PSObject.Properties.Name -contains "binaryDir") {
        $binaryDir = $preset.binaryDir
    }
    if (-not $binaryDir) {
        return (Join-Path $RepoRoot ("build\" + $PresetName))
    }
    $binaryDir = $binaryDir.Replace('${sourceDir}', $RepoRoot)
    $binaryDir = $binaryDir.Replace('${presetName}', $PresetName)
    if ([System.IO.Path]::IsPathRooted($binaryDir)) {
        return $binaryDir
    }
    return (Join-Path $RepoRoot $binaryDir)
}

$BuildDir = Resolve-BuildDir $ConfigurePreset
$CachePath = Join-Path $BuildDir "CMakeCache.txt"

if ($Action -eq "clean") {
    if (Test-Path $BuildDir) {
        Warn "Cleaning build directory: $BuildDir"
        Remove-Item -Recurse -Force $BuildDir
        Ok "Build directory cleaned"
    } else {
        Warn "Build directory does not exist: $BuildDir"
    }
    exit 0
}

if ($Action -eq "configure") {
    Info "Configuring CMake..."
    & cmake --preset $ConfigurePreset
    Ok "Configure completed"
    exit 0
}

if (-not (Test-Path $CachePath)) {
    Info "Configuring CMake..."
    & cmake --preset $ConfigurePreset
}

if ($Action -ne "run") {
    Info "Building..."
    $buildArgs = @("--build", "--preset", $BuildPreset)
    if ($Jobs) { $buildArgs += @("--parallel", $Jobs) }
    & cmake @buildArgs
}

if ($Action -ne "build") {
    $exeCandidates = @(
        (Join-Path $BuildDir "kicad\kicad.exe"),
        (Join-Path $BuildDir "bin\kicad.exe"),
        (Join-Path $BuildDir "kicad.exe")
    )
    $KicadExe = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $KicadExe) {
        Err "kicad.exe not found in build output. Checked: $($exeCandidates -join ', ')"
        exit 1
    }
    $env:KICAD_RUN_FROM_BUILD_DIR = "1"

    if ($AgentPanelDevUrl) {
        $env:KICAD_AGENT_PANEL_DEV_URL = $AgentPanelDevUrl
    } elseif ($AgentPanelDev) {
        $env:KICAD_AGENT_PANEL_DEV = "1"
    }

    Info "Agent panel dev URL: $($env:KICAD_AGENT_PANEL_DEV_URL)"

    Info "Running: $KicadExe"
    & $KicadExe
}

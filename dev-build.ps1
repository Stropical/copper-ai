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
$BuildPresetExplicit = $false
$ConfigurePresetExplicit = $false

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
            $BuildPresetExplicit = $true
            $i++
            continue
        }
        "^--configure-preset$" {
            if ($i + 1 -ge $args.Count) { Err "Missing value after --configure-preset"; exit 1 }
            $ConfigurePreset = $args[$i + 1]
            $ConfigurePresetExplicit = $true
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

function Resolve-ConfigurePresetFromBuildPreset([string]$BuildPresetName) {
    $presetJson = Get-Content -Raw -Path $PresetsPath | ConvertFrom-Json
    $buildPreset = $presetJson.buildPresets | Where-Object { $_.name -eq $BuildPresetName } | Select-Object -First 1
    if (-not $buildPreset) {
        return $null
    }
    if ($buildPreset.PSObject.Properties.Name -contains "configurePreset") {
        return $buildPreset.configurePreset
    }
    return $null
}

function Resolve-BuildPresetFromConfigurePreset([string]$ConfigurePresetName) {
    $presetJson = Get-Content -Raw -Path $PresetsPath | ConvertFrom-Json
    $buildPreset = $presetJson.buildPresets | Where-Object { $_.configurePreset -eq $ConfigurePresetName } | Select-Object -First 1
    if (-not $buildPreset) {
        return $null
    }
    return $buildPreset.name
}

if ($BuildPresetExplicit -and -not $ConfigurePresetExplicit) {
    $mappedConfigurePreset = Resolve-ConfigurePresetFromBuildPreset $BuildPreset
    if ($mappedConfigurePreset) {
        $ConfigurePreset = $mappedConfigurePreset
    } else {
        Warn "Build preset '$BuildPreset' not found; using configure preset '$ConfigurePreset'."
    }
} elseif ($ConfigurePresetExplicit -and -not $BuildPresetExplicit) {
    $mappedBuildPreset = Resolve-BuildPresetFromConfigurePreset $ConfigurePreset
    if ($mappedBuildPreset) {
        $BuildPreset = $mappedBuildPreset
    } else {
        Warn "Configure preset '$ConfigurePreset' has no matching build preset; using build preset '$BuildPreset'."
    }
} elseif ($BuildPresetExplicit -and $ConfigurePresetExplicit) {
    $mappedConfigurePreset = Resolve-ConfigurePresetFromBuildPreset $BuildPreset
    if ($mappedConfigurePreset -and ($mappedConfigurePreset -ne $ConfigurePreset)) {
        Warn "Build preset '$BuildPreset' expects configure preset '$mappedConfigurePreset', but '$ConfigurePreset' was provided."
    }
}

Info "Repository root: $RepoRoot"
Info "Configure preset: $ConfigurePreset"
Info "Build preset: $BuildPreset"

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
        $visited = @{}
        $stack = New-Object System.Collections.Generic.Stack[object]
        $stack.Push($preset)
        while ($stack.Count -gt 0) {
            $current = $stack.Pop()
            if (-not $current -or -not $current.name) { continue }
            if ($visited.ContainsKey($current.name)) { continue }
            $visited[$current.name] = $true

            if ($current.PSObject.Properties.Name -contains "binaryDir") {
                $binaryDir = $current.binaryDir
                break
            }

            if ($current.PSObject.Properties.Name -contains "inherits") {
                $inherits = $current.inherits
                if ($inherits -is [string]) {
                    $inherits = @($inherits)
                }
                foreach ($parentName in $inherits) {
                    $parent = $presetJson.configurePresets | Where-Object { $_.name -eq $parentName } | Select-Object -First 1
                    if ($parent) {
                        $stack.Push($parent)
                    }
                }
            }
        }
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

function Find-DllInPath([string]$DllName) {
    $pathEntries = ($env:PATH -split ';') | Where-Object { $_ -and ($_ -ne '.') }
    foreach ($entry in $pathEntries) {
        $trimmed = $entry.Trim()
        if (-not $trimmed) { continue }
        $candidate = Join-Path $trimmed $DllName
        if (Test-Path $candidate) {
            return $candidate
        }
    }
    return $null
}

function Prepend-PathEntry([string]$Entry) {
    if (-not $Entry) { return }
    if (-not (Test-Path $Entry)) { return }
    $current = $env:PATH -split ';'
    if ($current -contains $Entry) { return }
    $env:PATH = ($Entry + ";" + $env:PATH)
}

function Set-KicadRunPaths([string]$BuildDir, [bool]$IsDebug) {
    $runtimeDirs = @(
        (Join-Path $BuildDir "kicad"),
        (Join-Path $BuildDir "common"),
        (Join-Path $BuildDir "api"),
        (Join-Path $BuildDir "common\gal"),
        (Join-Path $BuildDir "pcbnew"),
        (Join-Path $BuildDir "eeschema"),
        (Join-Path $BuildDir "cvpcb")
    )

    $vcpkgRoot = Join-Path $BuildDir "vcpkg_installed\x64-windows"
    $vcpkgBin = if ($IsDebug) { Join-Path $vcpkgRoot "debug\bin" } else { Join-Path $vcpkgRoot "bin" }

    # Ensure build output and vcpkg bins are on PATH for DLL resolution.
    if (Test-Path $vcpkgBin) {
        Prepend-PathEntry $vcpkgBin
    }
    foreach ($dir in $runtimeDirs) {
        Prepend-PathEntry $dir
    }

    $existing = @()
    foreach ($dir in $runtimeDirs) {
        if (Test-Path $dir) { $existing += $dir }
    }
    $env:KICAD_BUILD_PATHS = ($existing -join ":")
}

function Add-PythonPathEntry([string]$Entry) {
    if (-not $Entry) { return }
    if (-not (Test-Path $Entry)) { return }
    $separator = ";"
    $current = @()
    if ($env:PYTHONPATH) {
        $current = $env:PYTHONPATH -split $separator
    }
    if ($current -contains $Entry) { return }
    if ($env:PYTHONPATH) {
        $env:PYTHONPATH = $Entry + $separator + $env:PYTHONPATH
    } else {
        $env:PYTHONPATH = $Entry
    }
}

function Ensure-KicadPythonEnv([string]$BuildDir) {
    $internalEncodings = Join-Path $BuildDir "kicad\\Lib\\encodings"
    if (Test-Path $internalEncodings) {
        return
    }

    if ($env:KICAD_USE_EXTERNAL_PYTHONHOME) {
        return
    }

    $vcpkgPythonHome = Join-Path $BuildDir "vcpkg_installed\\x64-windows\\tools\\python3"
    $vcpkgEncodings = Join-Path $vcpkgPythonHome "Lib\\encodings"
    if (-not (Test-Path $vcpkgEncodings)) {
        Warn "Python stdlib not found at $internalEncodings and vcpkg python not found at $vcpkgPythonHome."
        return
    }

    $env:KICAD_USE_EXTERNAL_PYTHONHOME = "1"
    if (-not $env:PYTHONHOME) {
        $env:PYTHONHOME = $vcpkgPythonHome
    }

    Add-PythonPathEntry (Join-Path $BuildDir "pcbnew")
    Add-PythonPathEntry (Join-Path $BuildDir "scripting")

    Info "Using external Python home: $($env:PYTHONHOME)"
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
    if ($LASTEXITCODE -ne 0) {
        Err "CMake configure failed."
        exit $LASTEXITCODE
    }
    Ok "Configure completed"
    exit 0
}

if (-not (Test-Path $CachePath)) {
    Info "Configuring CMake..."
    & cmake --preset $ConfigurePreset
    if ($LASTEXITCODE -ne 0) {
        Err "CMake configure failed."
        exit $LASTEXITCODE
    }
}

if ($Action -ne "run") {
    Info "Building..."
    $buildArgs = @("--build", "--preset", $BuildPreset)
    if ($Jobs) { $buildArgs += @("--parallel", $Jobs) }
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) {
        Err "CMake build failed."
        exit $LASTEXITCODE
    }
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
    $isDebugPreset = ($BuildPreset -match "debug") -or ($ConfigurePreset -match "debug")
    if ($isDebugPreset) {
        $missingDlls = @()
        foreach ($dll in @("MSVCP140D.dll", "VCRUNTIME140D.dll", "VCRUNTIME140_1D.dll", "ucrtbased.dll")) {
            if (-not (Find-DllInPath $dll)) { $missingDlls += $dll }
        }
        if ($missingDlls.Count -gt 0) {
            Err ("Debug CRT not found on PATH: " + ($missingDlls -join ", "))
            Warn "Install VS Build Tools (Desktop development with C++) or use --configure-preset msvc-win64-release --build-preset win64-release."
            exit 1
        }
    }
    $env:KICAD_RUN_FROM_BUILD_DIR = "1"
    Set-KicadRunPaths -BuildDir $BuildDir -IsDebug $isDebugPreset
    Ensure-KicadPythonEnv -BuildDir $BuildDir

    if ($AgentPanelDevUrl) {
        $env:KICAD_AGENT_PANEL_DEV_URL = $AgentPanelDevUrl
    } elseif ($AgentPanelDev) {
        $env:KICAD_AGENT_PANEL_DEV = "1"
    }

    Info "Agent panel dev URL: $($env:KICAD_AGENT_PANEL_DEV_URL)"

    Info "Running: $KicadExe"
    & $KicadExe
}

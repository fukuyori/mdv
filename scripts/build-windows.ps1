param(
    [string]$BuildDir = "build-windows-release",
    [string]$DeployDir = "dist\mdv-windows-x64",
    [string]$Config = "Release",
    [string]$QtDir,
    [string]$CMake,
    [string]$Generator,
    [switch]$NoDeploy
)

$ErrorActionPreference = "Stop"

function Resolve-RootDir {
    $scriptDir = Split-Path -Parent $PSCommandPath
    return (Resolve-Path (Join-Path $scriptDir "..")).Path
}

function Resolve-Tool {
    param(
        [string]$Name,
        [string[]]$Candidates = @()
    )

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command.Source
    }

    return $null
}

function Resolve-QtPrefixFromPath {
    param([string]$Path)

    if (-not $Path) {
        return $null
    }

    $candidate = [System.IO.Path]::GetFullPath($Path)
    if (Test-Path (Join-Path $candidate "bin\windeployqt.exe")) {
        return $candidate
    }

    if ($candidate -match "\\lib\\cmake\\Qt6$" -or $candidate -match "\\lib\\cmake\\Qt5$") {
        $prefix = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $candidate))
        if (Test-Path (Join-Path $prefix "bin\windeployqt.exe")) {
            return $prefix
        }
    }

    return $null
}

function Find-QtPrefix {
    param([string]$ExplicitQtDir)

    $candidates = @()
    if ($ExplicitQtDir) {
        $candidates += $ExplicitQtDir
    }
    if ($env:QT_DIR) {
        $candidates += $env:QT_DIR
    }
    if ($env:QTDIR) {
        $candidates += $env:QTDIR
    }
    if ($env:CMAKE_PREFIX_PATH) {
        $candidates += $env:CMAKE_PREFIX_PATH -split ";"
    }

    foreach ($candidate in $candidates) {
        $prefix = Resolve-QtPrefixFromPath $candidate
        if ($prefix) {
            return $prefix
        }
    }

    if (Test-Path "C:\Qt") {
        $qtKits = Get-ChildItem "C:\Qt" -Directory |
            ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
            Where-Object {
                $_.Name -match "^(msvc|mingw).+_64$" -and
                (Test-Path (Join-Path $_.FullName "bin\windeployqt.exe"))
            } |
            Sort-Object @{ Expression = { if ($_.Name -like "msvc*") { 0 } else { 1 } } }, FullName -Descending

        $kit = $qtKits | Select-Object -First 1
        if ($kit) {
            return $kit.FullName
        }
    }

    throw "Qt for Windows was not found. Pass -QtDir, set QT_DIR, or install Qt under C:\Qt."
}

function Find-VisualStudioVcVars {
    $candidates = @()
    if ($env:VSINSTALLDIR) {
        $candidates += (Join-Path $env:VSINSTALLDIR "VC\Auxiliary\Build\vcvars64.bat")
    }

    $vswhere = Resolve-Tool "vswhere.exe" @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    if ($vswhere) {
        $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installationPath) {
            $candidates += (Join-Path $installationPath "VC\Auxiliary\Build\vcvars64.bat")
        }
    }

    $candidates += @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    return $null
}

function Import-VcVars {
    param([string]$VcVars)

    Write-Host "Importing Visual Studio environment: $VcVars"
    $cmd = "call `"$VcVars`" amd64 >nul && set"
    $lines = & cmd.exe /d /s /c $cmd
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $lines) {
        if ($line -match "^([^=]+)=(.*)$") {
            $name = $Matches[1]
            if ($seen.Add($name)) {
                Set-Item -Path "env:$name" -Value $Matches[2]
            }
        }
    }
}

function Find-MingwBin {
    if (Test-Path "C:\Qt\Tools") {
        $gpp = Get-ChildItem "C:\Qt\Tools" -Directory -Filter "mingw*_64" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            ForEach-Object { Join-Path $_.FullName "bin\g++.exe" } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1
        if ($gpp) {
            return Split-Path -Parent $gpp
        }
    }

    $command = Get-Command "g++.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return Split-Path -Parent $command.Source
    }

    return $null
}

function Get-BuiltExecutable {
    param(
        [string]$BuildPath,
        [string]$BuildConfig
    )

    $candidates = @(
        (Join-Path $BuildPath "$BuildConfig\mdv.exe"),
        (Join-Path $BuildPath "mdv.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $found = Get-ChildItem $BuildPath -Recurse -Filter "mdv.exe" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($found) {
        return $found.FullName
    }

    throw "Built executable was not found under $BuildPath."
}

$rootDir = Resolve-RootDir
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $rootDir $BuildDir))
$deployPath = [System.IO.Path]::GetFullPath((Join-Path $rootDir $DeployDir))
$qtPrefix = Find-QtPrefix $QtDir
$cmakeBin = if ($CMake) { Resolve-Tool "cmake.exe" @($CMake) } else { Resolve-Tool "cmake.exe" @(
    "C:\Qt\Tools\CMake_64\bin\cmake.exe",
    "${env:ProgramFiles}\CMake\bin\cmake.exe"
) }
if (-not $cmakeBin) {
    throw "cmake.exe was not found. Install CMake or pass -CMake."
}

$ninja = Resolve-Tool "ninja.exe" @("C:\Qt\Tools\Ninja\ninja.exe")
if (-not $Generator -and $ninja) {
    $Generator = "Ninja"
}

$env:PATH = "$(Join-Path $qtPrefix "bin");$env:PATH"
$msvcKit = (Split-Path -Leaf $qtPrefix) -like "msvc*"
if ((Split-Path -Leaf $qtPrefix) -like "mingw*") {
    $mingwBin = Find-MingwBin
    if (-not $mingwBin) {
        throw "A MinGW Qt kit was selected, but g++.exe was not found. Install Qt's MinGW tools or pass an MSVC -QtDir."
    }
    $env:PATH = "$mingwBin;$env:PATH"
} elseif ($msvcKit -and -not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
    $vcVars = Find-VisualStudioVcVars
    if ($vcVars) {
        Import-VcVars $vcVars
    } elseif (-not $Generator) {
        Write-Warning "MSVC environment was not found. CMake may still work if another generator is configured."
    }
}

$configureArgs = @(
    "-S", $rootDir,
    "-B", $buildPath,
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_PREFIX_PATH=$qtPrefix"
)
if ($msvcKit) {
    $cl = Get-Command "cl.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $cl) {
        throw "An MSVC Qt kit was selected, but cl.exe was not found. Install Visual Studio C++ tools or pass a MinGW -QtDir."
    }
    $configureArgs += @(
        "-DCMAKE_C_COMPILER=$($cl.Source)",
        "-DCMAKE_CXX_COMPILER=$($cl.Source)"
    )
}
if ($Generator) {
    $configureArgs = @("-G", $Generator) + $configureArgs
    if ($Generator -eq "Ninja" -and $ninja) {
        $configureArgs += "-DCMAKE_MAKE_PROGRAM=$ninja"
    }
}

Write-Host "Configuring mdv ($Config)"
Write-Host "  Qt:    $qtPrefix"
Write-Host "  CMake: $cmakeBin"
if ($Generator) {
    Write-Host "  Generator: $Generator"
}
& $cmakeBin @configureArgs

Write-Host "Building mdv"
& $cmakeBin --build $buildPath --config $Config --parallel

$exePath = Get-BuiltExecutable $buildPath $Config
Write-Host "Built: $exePath"

if (-not $NoDeploy) {
    $windeployqt = Join-Path $qtPrefix "bin\windeployqt.exe"
    if (-not (Test-Path $windeployqt)) {
        throw "windeployqt.exe was not found in $qtPrefix\bin."
    }

    New-Item -ItemType Directory -Force -Path $deployPath | Out-Null
    Copy-Item -Force $exePath (Join-Path $deployPath "mdv.exe")
    $deployedExe = Join-Path $deployPath "mdv.exe"

    Write-Host "Deploying Qt runtime"
    & $windeployqt --release --compiler-runtime $deployedExe
    Write-Host "Deployed: $deployPath"
}

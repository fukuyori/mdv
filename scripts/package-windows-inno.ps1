param(
    [string]$BuildDir = "build-windows-release",
    [string]$DeployDir = "dist\mdv-windows-x64",
    [string]$WorkDir = "build-inno-installer",
    [string]$OutputDir = "dist",
    [string]$Config = "Release",
    [string]$QtDir,
    [string]$InnoSetupCompiler,
    [string]$Publisher = "fukuyori",
    [switch]$Build,
    [switch]$SkipBuild,
    [switch]$GenerateOnly
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

function Get-ProjectVersion {
    param([string]$CMakeLists)

    $content = Get-Content $CMakeLists -Raw
    if ($content -match "project\s*\(\s*mdv\s+VERSION\s+([0-9]+(?:\.[0-9]+){1,3})") {
        return $Matches[1]
    }

    throw "Could not read mdv project version from $CMakeLists."
}

function ConvertTo-InnoLiteral {
    param([string]$Value)
    return '"' + ($Value -replace '"', '""') + '"'
}

function Find-InnoCompiler {
    param([string]$ExplicitCompiler)

    $candidates = @()
    if ($ExplicitCompiler) {
        $candidates += $ExplicitCompiler
    }
    if ($env:ISCC) {
        $candidates += $env:ISCC
    }
    $candidates += @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 5\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 5\ISCC.exe"
    )

    return Resolve-Tool "ISCC.exe" $candidates
}

$rootDir = Resolve-RootDir
$deployPath = [System.IO.Path]::GetFullPath((Join-Path $rootDir $DeployDir))
$workPath = [System.IO.Path]::GetFullPath((Join-Path $rootDir $WorkDir))
$outputPath = [System.IO.Path]::GetFullPath((Join-Path $rootDir $OutputDir))
$issPath = Join-Path $workPath "mdv.iss"
$version = Get-ProjectVersion (Join-Path $rootDir "CMakeLists.txt")

if ($Build -and -not $SkipBuild) {
    $buildScript = Join-Path $rootDir "scripts\build-windows.ps1"
    $buildArgs = @{
        BuildDir = $BuildDir
        DeployDir = $DeployDir
        Config = $Config
    }
    if ($QtDir) {
        $buildArgs.QtDir = $QtDir
    }
    & $buildScript @buildArgs
}

if (-not (Test-Path (Join-Path $deployPath "mdv.exe"))) {
    throw "The deployed payload was not found at $deployPath. Run scripts\build-windows.ps1 first, or pass -Build to rebuild from the packaging script."
}

New-Item -ItemType Directory -Force -Path $workPath | Out-Null
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

$readme = Join-Path $rootDir "README.md"
$readmeJa = Join-Path $rootDir "README.ja.md"
$icon = Join-Path $rootDir "resources\windows\mdv.ico"
if (Test-Path $readme) {
    Copy-Item -Force $readme (Join-Path $deployPath "README.md")
}
if (Test-Path $readmeJa) {
    Copy-Item -Force $readmeJa (Join-Path $deployPath "README.ja.md")
}

$payloadLiteral = ConvertTo-InnoLiteral $deployPath
$outputLiteral = ConvertTo-InnoLiteral $outputPath
$publisherLiteral = ConvertTo-InnoLiteral $Publisher
$iconLiteral = ConvertTo-InnoLiteral $icon

$iss = @"
#define MyAppName "mdv"
#define MyAppVersion "$version"
#define MyAppPublisher $publisherLiteral
#define PayloadDir $payloadLiteral
#define IconFile $iconLiteral

[Setup]
AppId={{A2534936-51D6-4F95-B31C-5C0709D68E31}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\mdv
DefaultGroupName=mdv
DisableProgramGroupPage=yes
OutputDir=$outputLiteral
OutputBaseFilename=mdiv-{#MyAppVersion}-windows-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\mdv.exe
SetupIconFile={#IconFile}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\mdv"; Filename: "{app}\mdv.exe"; IconFilename: "{app}\mdv.exe"
Name: "{autodesktop}\mdv"; Filename: "{app}\mdv.exe"; IconFilename: "{app}\mdv.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: waituntilterminated skipifdoesntexist
Filename: "{app}\mdv.exe"; Description: "{cm:LaunchProgram,mdv}"; Flags: nowait postinstall skipifsilent
"@

Set-Content -Path $issPath -Value $iss -Encoding UTF8
Write-Host "Generated: $issPath"

if ($GenerateOnly) {
    Write-Host "Skipped Inno Setup compile because -GenerateOnly was specified."
    return
}

$iscc = Find-InnoCompiler $InnoSetupCompiler
if (-not $iscc) {
    throw "ISCC.exe was not found. Install Inno Setup 6, set ISCC, pass -InnoSetupCompiler, or use -GenerateOnly."
}

Write-Host "Compiling installer with Inno Setup"
& $iscc $issPath
Write-Host "Installer output: $(Join-Path $outputPath "mdiv-$version-windows-x64.exe")"

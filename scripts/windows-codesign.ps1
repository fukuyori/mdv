# Shared Authenticode signing helpers for the Windows build/packaging scripts.
#
# The signing certificate is taken from the CODESIGN_CERT environment variable:
#   * a path to a .pfx/.p12/.cer file (password from CODESIGN_CERT_PASSWORD)
#   * a certificate SHA1 thumbprint from the current user/machine store
#   * a certificate subject name from the current user/machine store
#
# Optional environment variables:
#   CODESIGN_CERT_PASSWORD  password for the certificate file
#   CODESIGN_TIMESTAMP_URL  RFC 3161 timestamp server (default: DigiCert)
#   CODESIGN_DIGEST         file/timestamp digest algorithm (default: sha256)
#   CODESIGN_CSP            cryptographic provider (hardware tokens)
#   CODESIGN_KEY_CONTAINER  key container name (hardware tokens)
#   SIGNTOOL                explicit path to signtool.exe

function Find-SignTool {
    param([string]$ExplicitSignTool)

    $candidates = @()
    if ($ExplicitSignTool) {
        $candidates += $ExplicitSignTool
    }
    if ($env:SIGNTOOL) {
        $candidates += $env:SIGNTOOL
    }

    $kitRoots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    )
    foreach ($root in $kitRoots) {
        if (Test-Path $root) {
            $candidates += Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName "x64\signtool.exe" }
        }
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    $command = Get-Command "signtool.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command.Source
    }

    return $null
}

function Get-CodeSignSettings {
    param(
        [string]$SignToolPath,
        [string]$TimestampUrl
    )

    $cert = $env:CODESIGN_CERT
    if (-not $cert) {
        throw "CODESIGN_CERT is not set. Point it at a .pfx file, a certificate SHA1 thumbprint, or a certificate subject name before using -Sign."
    }

    $signtool = Find-SignTool $SignToolPath
    if (-not $signtool) {
        throw "signtool.exe was not found. Install the Windows SDK signing tools, set SIGNTOOL, or pass -SignToolPath."
    }

    $digest = if ($env:CODESIGN_DIGEST) { $env:CODESIGN_DIGEST } else { "sha256" }
    $timestamp = $TimestampUrl
    if (-not $timestamp) {
        $timestamp = $env:CODESIGN_TIMESTAMP_URL
    }
    if (-not $timestamp) {
        $timestamp = "http://timestamp.digicert.com"
    }

    $arguments = @("sign", "/fd", $digest)
    $description = $cert

    if (Test-Path $cert) {
        $certPath = (Resolve-Path $cert).Path
        $description = $certPath
        $arguments += @("/f", $certPath)
        if ($env:CODESIGN_CERT_PASSWORD) {
            $arguments += @("/p", $env:CODESIGN_CERT_PASSWORD)
        }
        if ($env:CODESIGN_CSP) {
            $arguments += @("/csp", $env:CODESIGN_CSP)
        }
        if ($env:CODESIGN_KEY_CONTAINER) {
            $arguments += @("/kc", $env:CODESIGN_KEY_CONTAINER)
        }
    } elseif ($cert -match "^[0-9A-Fa-f][0-9A-Fa-f\s]{38,}$") {
        $thumbprint = $cert -replace "\s", ""
        $description = "thumbprint $thumbprint"
        $arguments += @("/sha1", $thumbprint)
    } else {
        $description = "subject `"$cert`""
        $arguments += @("/n", $cert)
    }

    $arguments += @("/tr", $timestamp, "/td", $digest)

    return [pscustomobject]@{
        SignTool = $signtool
        Arguments = $arguments
        Certificate = $description
        TimestampUrl = $timestamp
    }
}

function Write-CodeSignSummary {
    param($Settings)

    Write-Host "Code signing enabled"
    Write-Host "  signtool:  $($Settings.SignTool)"
    Write-Host "  cert:      $($Settings.Certificate)"
    Write-Host "  timestamp: $($Settings.TimestampUrl)"
}

function Invoke-CodeSign {
    param(
        [Parameter(Mandatory = $true)]$Settings,
        [Parameter(Mandatory = $true)][string[]]$Paths
    )

    $targets = @()
    foreach ($path in $Paths) {
        if (-not (Test-Path $path)) {
            throw "Cannot sign a file that does not exist: $path"
        }
        $targets += (Resolve-Path $path).Path
    }

    if (-not $targets) {
        return
    }

    Write-Host "Signing: $($targets -join ', ')"
    & $Settings.SignTool @($Settings.Arguments) @targets
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed with exit code $LASTEXITCODE."
    }
}

# Builds the command line handed to Inno Setup via ISCC /S<name>=<command>, so
# that ISCC signs the installer and (with SignedUninstaller=yes) the uninstaller.
function Get-InnoSignToolCommand {
    param($Settings)

    $parts = @('$q' + $Settings.SignTool + '$q')
    foreach ($argument in $Settings.Arguments) {
        if ($argument -match "\s") {
            $parts += '$q' + $argument + '$q'
        } else {
            $parts += $argument
        }
    }
    $parts += '$f'

    return ($parts -join " ")
}

<#
.SYNOPSIS
    Configure and build Saber in development mode (using local extern/ dependencies)

.DESCRIPTION
    This script:
    1. Swaps vcpkg-configuration.json with the dev version (for overlay-ports)
    2. Runs cmake configure with the 'dev' preset
    3. Builds the project
    4. Restores the original vcpkg-configuration.json

.PARAMETER BuildType
    CMake build type: Debug (default), RelWithDebInfo, or Release

.PARAMETER Clean
    Clean the build directory before configuring

.EXAMPLE
    .\build-dev.ps1
    .\build-dev.ps1 -BuildType RelWithDebInfo
    .\build-dev.ps1 -Clean
#>

param(
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$BuildType = "Debug",

    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot

# Paths
$VcpkgConfig = Join-Path $ProjectRoot "vcpkg-configuration.json"
$VcpkgConfigDev = Join-Path $ProjectRoot "vcpkg-configuration.dev.json"
$VcpkgConfigBackup = Join-Path $ProjectRoot "vcpkg-configuration.json.bak"
$BuildDir = Join-Path $ProjectRoot "build"

# Verify dev config exists
if (-not (Test-Path $VcpkgConfigDev)) {
    Write-Error "vcpkg-configuration.dev.json not found. Please create it first."
    exit 1
}

# Verify extern junctions exist
$ExternEkizu = Join-Path $ProjectRoot "extern/ekizu"
$ExternYtdlpp = Join-Path $ProjectRoot "extern/ytdlpp"

if (-not (Test-Path $ExternEkizu)) {
    Write-Warning "extern/ekizu not found. Create a junction with:"
    Write-Host "  cmd /c mklink /J `"$ExternEkizu`" `"<path-to-ekizu-repo>`""
}

if (-not (Test-Path $ExternYtdlpp)) {
    Write-Warning "extern/ytdlpp not found. Create a junction with:"
    Write-Host "  cmd /c mklink /J `"$ExternYtdlpp`" `"<path-to-ytdlpp-repo>`""
}

try {
    # Backup and swap vcpkg config
    Write-Host "Swapping to dev vcpkg configuration..." -ForegroundColor Cyan
    if (Test-Path $VcpkgConfig) {
        Copy-Item $VcpkgConfig $VcpkgConfigBackup -Force
    }
    Copy-Item $VcpkgConfigDev $VcpkgConfig -Force

    # Clean if requested
    if ($Clean -and (Test-Path $BuildDir)) {
        Write-Host "Cleaning build directory..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $BuildDir
    }

    # Configure
    Write-Host "Configuring with CMake (BuildType: $BuildType)..." -ForegroundColor Cyan
    $ConfigArgs = @(
        "-S", $ProjectRoot,
        "-B", $BuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DVCPKG_TARGET_TRIPLET=x64-windows",
        "-DVCPKG_OVERLAY_PORTS=$ExternEkizu/overlay-ports;$ExternYtdlpp/overlay-ports"
    )

    # Add toolchain file if VCPKG_ROOT is set
    if ($env:VCPKG_ROOT) {
        $ConfigArgs += "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
    } elseif (Test-Path "C:/Users/$env:USERNAME/scoop/apps/vcpkg/current/scripts/buildsystems/vcpkg.cmake") {
        $ConfigArgs += "-DCMAKE_TOOLCHAIN_FILE=C:/Users/$env:USERNAME/scoop/apps/vcpkg/current/scripts/buildsystems/vcpkg.cmake"
    } else {
        Write-Error "VCPKG_ROOT not set and vcpkg not found in scoop. Please set VCPKG_ROOT."
        exit 1
    }

    cmake @ConfigArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

    # Build
    Write-Host "Building..." -ForegroundColor Cyan
    cmake --build $BuildDir --config $BuildType
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

    Write-Host "Build complete!" -ForegroundColor Green
}
finally {
    # Always restore original config
    if (Test-Path $VcpkgConfigBackup) {
        Write-Host "Restoring original vcpkg configuration..." -ForegroundColor Cyan
        Move-Item $VcpkgConfigBackup $VcpkgConfig -Force
    }
}

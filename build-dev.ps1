<#
.SYNOPSIS
    Configure and build Saber in development mode (using local extern/ dependencies)

.DESCRIPTION
    This script:
    1. Swaps vcpkg-configuration.json with the dev version (for inner dependencies)
    2. Runs cmake configure with SABER_DEV_MODE=ON (uses add_subdirectory)
    3. Builds the project
    4. Restores the original vcpkg-configuration.json

    Using add_subdirectory instead of vcpkg overlay-ports means:
    - Incremental builds: only changed files in ekizu/ytdlpp are recompiled
    - No full package rebuild when you change one line

.PARAMETER BuildType
    CMake build type: Debug (default), RelWithDebInfo, or Release

.PARAMETER Clean
    Clean the build directory before configuring

.PARAMETER Configure
    Only configure, don't build

.EXAMPLE
    .\build-dev.ps1
    .\build-dev.ps1 -BuildType RelWithDebInfo
    .\build-dev.ps1 -Clean
    .\build-dev.ps1 -Configure  # Only configure, useful for IDE integration
#>

param(
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$BuildType = "Debug",

    [switch]$Clean,
    [switch]$Configure
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

if (-not (Test-Path "$ExternEkizu/CMakeLists.txt")) {
    Write-Error @"
extern/ekizu not found or is not a valid repo. Create a junction with:
  cmd /c mklink /J "$ExternEkizu" "<path-to-ekizu-repo>"
"@
    exit 1
}

if (-not (Test-Path "$ExternYtdlpp/CMakeLists.txt")) {
    Write-Error @"
extern/ytdlpp not found or is not a valid repo. Create a junction with:
  cmd /c mklink /J "$ExternYtdlpp" "<path-to-ytdlpp-repo>"
"@
    exit 1
}

try {
    # Backup and swap vcpkg config (needed for inner dependencies like libdave, mlspp)
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

    # Determine vcpkg toolchain location
    $VcpkgToolchain = $null
    if ($env:VCPKG_ROOT) {
        $VcpkgToolchain = "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
    } elseif (Test-Path "C:/Users/$env:USERNAME/scoop/apps/vcpkg/current/scripts/buildsystems/vcpkg.cmake") {
        $VcpkgToolchain = "C:/Users/$env:USERNAME/scoop/apps/vcpkg/current/scripts/buildsystems/vcpkg.cmake"
    } else {
        Write-Error "VCPKG_ROOT not set and vcpkg not found in scoop. Please set VCPKG_ROOT."
        exit 1
    }

    # Configure with SABER_DEV_MODE
    Write-Host "Configuring with CMake (BuildType: $BuildType, DEV_MODE: ON)..." -ForegroundColor Cyan
    $ConfigArgs = @(
        "-S", $ProjectRoot,
        "-B", $BuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DVCPKG_TARGET_TRIPLET=x64-windows",
        "-DSABER_DEV_MODE=ON",
        "-DVCPKG_OVERLAY_PORTS=$ExternEkizu/overlay-ports;$ExternYtdlpp/overlay-ports",
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
    )

    cmake @ConfigArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

    if (-not $Configure) {
        # Build
        Write-Host "Building..." -ForegroundColor Cyan
        cmake --build $BuildDir --config $BuildType
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
        Write-Host "Build complete!" -ForegroundColor Green
    } else {
        Write-Host "Configure complete! (skipped build due to -Configure flag)" -ForegroundColor Green
    }
}
finally {
    # Always restore original config
    if (Test-Path $VcpkgConfigBackup) {
        Write-Host "Restoring original vcpkg configuration..." -ForegroundColor Cyan
        Move-Item $VcpkgConfigBackup $VcpkgConfig -Force
    }
}

Write-Host @"

Next steps:
  - Incremental build: cmake --build build
  - Run the bot:       .\build\bin\saber.exe
  - Clean rebuild:     .\build-dev.ps1 -Clean

"@ -ForegroundColor DarkGray

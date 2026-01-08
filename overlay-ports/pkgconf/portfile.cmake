# pkgconf overlay port - uses system pkgconf instead of building This avoids
# QEMU emulation issues when cross-compiling for arm64

# Verify system pkgconf is available
find_program(PKGCONF_EXECUTABLE NAMES pkgconf pkg-config REQUIRED)
message(STATUS "Using system pkgconf: ${PKGCONF_EXECUTABLE}")

# Get version
execute_process(
    COMMAND ${PKGCONF_EXECUTABLE} --version
    OUTPUT_VARIABLE PKGCONF_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "System pkgconf version: ${PKGCONF_VERSION}")

# Create the pkgconf wrapper script that vcpkg expects
set(PKGCONF_WRAPPER "${CURRENT_PACKAGES_DIR}/tools/pkgconf/pkgconf")
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/pkgconf")

# Create a simple wrapper script
file(WRITE "${PKGCONF_WRAPPER}"
     "#!/bin/sh\nexec ${PKGCONF_EXECUTABLE} \"$@\"\n"
)

# Make it executable
file(
    CHMOD
    "${PKGCONF_WRAPPER}"
    FILE_PERMISSIONS
    OWNER_READ
    OWNER_WRITE
    OWNER_EXECUTE
    GROUP_READ
    GROUP_EXECUTE
    WORLD_READ
    WORLD_EXECUTE
)

# Also create pkg-config symlink
file(CREATE_LINK "${PKGCONF_WRAPPER}"
     "${CURRENT_PACKAGES_DIR}/tools/pkgconf/pkg-config" SYMBOLIC
)

# Create usage file
file(
    WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage"
    "pkgconf is provided by the system installation.
The wrapper script delegates to: ${PKGCONF_EXECUTABLE}
"
)

# Create copyright file
file(
    WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright"
    "This overlay port uses the system-installed pkgconf.
See system package for license information.
"
)

# Mark as empty package (header-only style)
set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

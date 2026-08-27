# Copyright (C) 2026 Honu Robotics
#
# Debian (.deb) packaging via CPack, component-based so we ship the standard
# library split:
#   libehukai0     runtime: libehukai.so.0, libehukai.so.0.0.1
#   libehukai-dev  headers, libehukai.so symlink, CMake package config
#
# The runtime package name tracks the SONAME, which CMakeLists.txt derives from
# PROJECT_VERSION_MAJOR; a major bump renames the package (libehukai0 -> 1).
#
# Build the packages with (umask 022 keeps directory perms at 0755):
#   umask 022
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
#         -DEHUKAI_ENABLE_DEB_PACKAGING=ON
#   cmake --build build -j"$(nproc)"
#   ( cd build && cpack -G DEB )
#
# EHUKAI_ENABLE_DEB_PACKAGING is required: this file is not included otherwise,
# because the files it installs are meaningful only inside these .debs.
#
# CMAKE_INSTALL_PREFIX=/usr is required, not optional. GNUInstallDirs only
# expands CMAKE_INSTALL_LIBDIR to the Debian multiarch path 

set(CPACK_PACKAGE_NAME      "ehukai")
set(CPACK_PACKAGE_VENDOR    "Honu Robotics")
set(CPACK_PACKAGE_CONTACT   "Honu Robotics <info@honurobotics.com>")
set(CPACK_PACKAGE_VERSION   "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Headless spectral ocean-wave synthesis C++ library")

set(CPACK_GENERATOR "DEB")
# Debian packages live under /usr (the DEB generator's default, pinned here for
# clarity). Pair with -DCMAKE_INSTALL_PREFIX=/usr at configure time so the
# multiarch libdir matches; see the header comment above.
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
set(CPACK_COMPONENTS_ALL runtime dev)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)            # libehukai0_0.0.1-1_amd64.deb
# Base Debian revision. CI appends a per-distribution suffix (e.g. ~ubuntu24.04)
# via -DEHUKAI_DEB_DISTRO_SUFFIX so the same upstream version coexists across Ubuntu
# releases and a release upgrade pulls the newer build (dpkg orders
# ~ubuntu24.04 < ~ubuntu26.04). Unset, packaging is byte-for-byte as before.
# The old EW_ spellings of these variables must fail loudly: CMake only
# warns about unused -D variables, which is how a renamed flag once shipped
# suffixless debs from a green build.
if(DEFINED EW_DEB_DISTRO_SUFFIX OR DEFINED EW_DEB_DISTRO_CODENAME)
  message(FATAL_ERROR "EW_DEB_DISTRO_* was renamed to EHUKAI_DEB_DISTRO_*; "
    "update the caller (see .github/workflows/release.yml).")
endif()
set(EHUKAI_DEB_DISTRO_SUFFIX "" CACHE STRING
    "Per-distribution Debian version suffix, e.g. ~ubuntu24.04")
set(EHUKAI_DEB_DISTRO_CODENAME "" CACHE STRING
    "Target distribution codename for the changelog top entry, e.g. noble")
set(_ew_deb_base_release 1)
set(CPACK_DEBIAN_PACKAGE_RELEASE "${_ew_deb_base_release}${EHUKAI_DEB_DISTRO_SUFFIX}")
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/HonuRobotics/ehukai")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)            # auto runtime deps via dpkg-shlibdeps
set(CPACK_STRIP_FILES TRUE)                       # strip the shared object
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)   # 0755 maintainer scripts

# Provide a versioned shlibs file so downstream CPack/dpkg-shlibdeps consumers can
# resolve a dependency on this library.
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY ">=")

# ---- runtime package: the shared object ----
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME    "libehukai0")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION
    "Spectral ocean-wave synthesis library (Tessendorf-style FFT), headless\nwith no OpenGL viewer, for embedding in simulators and tools. This\npackage contains the shared runtime library.")
# Route ldconfig through a modern dpkg trigger. The postinst/postrm are supplied
# (rather than letting CPack auto-generate ones that call ldconfig directly) so the
# trigger is the single ldconfig mechanism; they intentionally do nothing else.
set(CPACK_DEBIAN_RUNTIME_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/triggers;${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/postinst;${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/postrm")

# ---- dev package: headers, .so symlink, CMake config ----
set(CPACK_DEBIAN_DEV_PACKAGE_NAME    "libehukai-dev")
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION "libdevel")
set(CPACK_COMPONENT_DEV_DEPENDS runtime)          # -> libehukai0 (= exact version)
# The PUBLIC link deps surface in the installed headers and in
# ehukaiConfig.cmake's find_dependency() calls, so downstream builds need
# their -dev packages. Floors reuse EHUKAI_*_MIN from the top-level CMakeLists.txt
# so the packaging metadata can never drift from the build-time requirements.
set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS
    "libeigen3-dev (>= ${EHUKAI_EIGEN3_MIN}), libtbb-dev (>= ${EHUKAI_TBB_MIN}), libimath-dev (>= ${EHUKAI_IMATH_MIN})")
set(CPACK_COMPONENT_DEV_DESCRIPTION
    "Spectral ocean-wave synthesis library (Tessendorf-style FFT), headless\nwith no OpenGL viewer, for embedding in simulators and tools. This\npackage contains the development headers and the CMake package config.")

# ---- Debian changelog (lintian requires a compressed changelog.Debian) ----
find_program(GZIP_TOOL gzip REQUIRED)
set(_changelog_gz "${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian.gz")

# The changelog is hand-maintained; fail loudly if its top entry drifts from
# the version we are actually packaging (PROJECT_VERSION-PACKAGE_RELEASE).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/changelog.Debian"
  _changelog_first LIMIT_COUNT 1)
if(NOT _changelog_first MATCHES "^ehukai \\(([^)]+)\\)")
  message(FATAL_ERROR
    "Cannot parse version from changelog.Debian first line: '${_changelog_first}'")
endif()
# Assert against the BASE revision: the hand-maintained source changelog stays
# release-agnostic; the distro suffix is stamped into the installed copy below.
set(_expected_changelog_version "${PROJECT_VERSION}-${_ew_deb_base_release}")
if(NOT CMAKE_MATCH_1 STREQUAL _expected_changelog_version)
  message(FATAL_ERROR
    "changelog.Debian version (${CMAKE_MATCH_1}) does not match the package "
    "version (${_expected_changelog_version}); update cmake/deb/changelog.Debian.")
endif()
# RESULT_VARIABLE rather than COMMAND_ERROR_IS_FATAL (CMake >= 3.19) to stay at
# the 3.16 floor; a failed gzip would otherwise silently ship an empty/truncated
# changelog.Debian.gz.
# Stamp the per-distribution suffix and codename into the *installed* changelog
# so its top entry matches the actual package version and target distribution
# (dpkg + lintian correctness).
set(_changelog_src "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/changelog.Debian")
if(EHUKAI_DEB_DISTRO_SUFFIX)
  file(READ "${_changelog_src}" _changelog_text)
  string(REPLACE
    "ehukai (${PROJECT_VERSION}-${_ew_deb_base_release})"
    "ehukai (${PROJECT_VERSION}-${_ew_deb_base_release}${EHUKAI_DEB_DISTRO_SUFFIX})"
    _changelog_text "${_changelog_text}")
  # Retarget the top entry's distribution to this build's codename (the source
  # changelog carries a single fixed codename). Rewrite the field up to the ';'
  # so it is agnostic to whatever codename the source file happens to name.
  if(EHUKAI_DEB_DISTRO_CODENAME)
    string(REGEX REPLACE
      "^(ehukai \\([^)]+\\)) +[^;]+;"
      "\\1 ${EHUKAI_DEB_DISTRO_CODENAME};"
      _changelog_text "${_changelog_text}")
  endif()
  set(_changelog_src "${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian.stamped")
  file(WRITE "${_changelog_src}" "${_changelog_text}")
endif()
execute_process(
  COMMAND ${GZIP_TOOL} -9nc "${_changelog_src}"
  OUTPUT_FILE "${_changelog_gz}"
  RESULT_VARIABLE _changelog_gz_result)
if(NOT _changelog_gz_result EQUAL 0)
  message(FATAL_ERROR
    "Failed to compress changelog.Debian (gzip exit ${_changelog_gz_result})")
endif()
install(FILES "${_changelog_gz}"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libehukai0    COMPONENT runtime)
install(FILES "${_changelog_gz}"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libehukai-dev COMPONENT dev)

# ---- machine-readable copyright (lintian requires one per binary package) ----
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/copyright"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libehukai0    COMPONENT runtime)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/copyright"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libehukai-dev COMPONENT dev)

# ---- lintian overrides for documented, internal-only acceptable tags ----
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/lintian-overrides-runtime"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/lintian/overrides
  RENAME libehukai0 COMPONENT runtime)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/lintian-overrides-dev"
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/lintian/overrides
  RENAME libehukai-dev COMPONENT dev)

include(CPack)   # MUST be last: consumes the CPACK_* variables set above

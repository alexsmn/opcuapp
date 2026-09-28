# Does a Release build link the RELEASE OpenSSL and libb2, and a Debug build
# the DEBUG ones, when the product configures with "Ninja Multi-Config"?
#
# Run with `cmake -P`, handing it a C compiler's generator:
#   cmake -DCMAKE_MAKE_PROGRAM=<ninja> -P test_single_config_libraries.cmake
#
# It configures (never builds) a two-directory project over a synthesized
# vcpkg installed tree: empty `libcrypto`/`libssl`/`libb2` archives under
# `<triplet>/lib` and `<triplet>/debug/lib`, and fake OpenSSL headers. The
# search order is the one vcpkg's toolchain produces when `CMAKE_BUILD_TYPE` is
# undefined -- debug prefix first -- and the find module is CMake's own
# FindOpenSSL, so the defect is reproduced rather than modelled. Each
# configuration's resolved library paths are written by `file(GENERATE)` and
# compared here.
#
# Two cases, because a fix test that cannot fail proves nothing:
# `unpinned` must reproduce the bug (Release gets `debug/lib`), and `pinned`,
# which runs the real `scada_pin_vcpkg_single_config_libraries()` from the kit,
# must not. Backlog 853: every product linked the debug OpenSSL in Release
# until 2026-09-27, and the client the debug libb2 too, with every suite green.

cmake_minimum_required(VERSION 3.25)

if(WIN32)
  # MSVC takes vcpkg's own per-config OpenSSL lookup, and the pin skips it.
  message(STATUS "skipped: the multi-config defect this pins is non-MSVC")
  return()
endif()

get_filename_component(_kit "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT SCADA_TEST_SCRATCH_DIR)
  set(SCADA_TEST_SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/single-config-test")
endif()
file(REMOVE_RECURSE "${SCADA_TEST_SCRATCH_DIR}")

# --- the synthesized vcpkg installed tree -----------------------------------

set(_installed "${SCADA_TEST_SCRATCH_DIR}/vcpkg_installed")
set(_triplet "fake-triplet")
foreach(_dir IN ITEMS "${_installed}/${_triplet}/lib"
                      "${_installed}/${_triplet}/debug/lib")
  foreach(_lib IN ITEMS crypto ssl b2)
    file(WRITE "${_dir}/lib${_lib}.a" "")
  endforeach()
endforeach()
file(WRITE "${_installed}/${_triplet}/include/openssl/ssl.h" "")
file(WRITE "${_installed}/${_triplet}/include/openssl/opensslv.h"
  "# define OPENSSL_VERSION_NUMBER 0x30500000L\n")

# --- the project --------------------------------------------------------------

set(_src "${SCADA_TEST_SCRATCH_DIR}/src")
file(COPY "${_kit}/ScadaProducts.cmake" "${_kit}/ScadaProductBase.cmake"
          "${_kit}/ScadaLocal.cmake"
     DESTINATION "${_src}/build-support")
file(WRITE "${_src}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.25)
project(single_config_pin LANGUAGES C)
# What vcpkg.cmake leaves behind with CMAKE_BUILD_TYPE undefined.
set(VCPKG_INSTALLED_DIR \"${_installed}\")
set(VCPKG_TARGET_TRIPLET ${_triplet})
list(PREPEND CMAKE_PREFIX_PATH
  \"${_installed}/${_triplet}/debug\" \"${_installed}/${_triplet}\")
# Keep a host OpenSSL out of it: FindOpenSSL's pkg-config hints come first.
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
if(PIN)
  include(build-support/ScadaProducts.cmake)
  include(build-support/ScadaProductBase.cmake)
  scada_pin_vcpkg_single_config_libraries()
endif()
# A subdirectory, as in every product: the pin runs at the root and the
# find_package calls happen below it.
add_subdirectory(sub)
")
file(WRITE "${_src}/sub/CMakeLists.txt" "
find_package(OpenSSL REQUIRED)
# Qt's FindLibb2.cmake, reduced to what matters: it returns early when the
# target exists, and otherwise does a single find_library.
if(NOT TARGET Libb2::Libb2)
  find_library(LIBB2_LIBRARY NAMES b2)
  add_library(Libb2::Libb2 UNKNOWN IMPORTED)
  set_target_properties(Libb2::Libb2 PROPERTIES IMPORTED_LOCATION \${LIBB2_LIBRARY})
endif()
file(GENERATE OUTPUT \"\${CMAKE_BINARY_DIR}/libs-$<CONFIG>.txt\" CONTENT
  \"$<TARGET_LINKER_FILE:OpenSSL::Crypto>;$<TARGET_LINKER_FILE:OpenSSL::SSL>;$<TARGET_LINKER_FILE:Libb2::Libb2>\")
")

# --- the cases ----------------------------------------------------------------

macro(configure_case name pin)
  set(_bin "${SCADA_TEST_SCRATCH_DIR}/build-${name}")
  set(_make_program)
  if(CMAKE_MAKE_PROGRAM)
    set(_make_program "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -S "${_src}" -B "${_bin}"
            -G "Ninja Multi-Config" ${_make_program} -DPIN=${pin}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${name}: configure failed:\n${_out}")
  endif()
endmacro()

# Every library resolved for `config` in case `name` lies in the release
# (`expect_debug` FALSE) or debug build.
macro(expect_libs name config expect_debug)
  file(READ "${SCADA_TEST_SCRATCH_DIR}/build-${name}/libs-${config}.txt" _libs)
  foreach(_lib IN LISTS _libs)
    if(_lib MATCHES "/debug/lib/")
      set(_is_debug TRUE)
    else()
      set(_is_debug FALSE)
    endif()
    if(_is_debug STREQUAL "${expect_debug}")
      message(STATUS "ok: ${name} ${config} -> ${_lib}")
    else()
      set(SCADA_TEST_FAILED TRUE)
      message(SEND_ERROR "${name} ${config}: got ${_lib}")
    endif()
  endforeach()
endmacro()

configure_case(unpinned OFF)
# The defect, reproduced: without the pin, Release links the debug archives.
expect_libs(unpinned Release TRUE)

configure_case(pinned ON)
expect_libs(pinned Release FALSE)
expect_libs(pinned RelWithDebInfo FALSE)
expect_libs(pinned Debug TRUE)

if(SCADA_TEST_FAILED)
  message(FATAL_ERROR "single-config library pinning is broken")
endif()

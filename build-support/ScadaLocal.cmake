# Machine-specific build settings — one file per machine, read by every product.
#
# Before ADR 0011 these lived in a git-ignored root `CMakeUserPresets.json`:
# vcpkg triplet, ccache launchers, cppcheck's location, and on Windows the whole
# MSVC `INCLUDE`/`LIB`/`PATH` environment. With 25 products each carrying its own
# self-contained presets, replicating that block 25 times is not tenable, so it
# moves to a single file beside `build-support/` — `.scada-local.cmake`, which is
# git-ignored — and every product includes it.
#
# It is optional. A product with no local file configures with plain defaults,
# which is what a customer building an export gets.
#
# The one machine input that CANNOT live here is `VCPKG_ROOT`: presets name the
# toolchain file, and the toolchain is processed before any of this runs. That
# stays an environment variable, as vcpkg intends.
#
# See docs/adr/0011-standalone-product-builds.md and
# build-support/scada-local.cmake.example.

include_guard(GLOBAL)

# The vcpkg toolchain a preset names, checked before CMake tries to load it.
#
# Every product's `ninja` preset sets
# `"toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"`, and
# CMake interpolates an unset variable to the empty string rather than
# complaining -- so a shell without `VCPKG_ROOT` asks for
# `/scripts/buildsystems/vcpkg.cmake` and gets three errors in a row, none of
# which mentions the variable:
#
#   Could not find toolchain file: "/scripts/buildsystems/vcpkg.cmake"
#   CMake was unable to find a build program corresponding to "Ninja Multi-Config"
#   CMAKE_CXX_COMPILER not set, after EnableLanguage
#
# and then leaves a ~100-line CMakeCache.txt behind, which reads as a
# configured tree at a glance and is reused by the next run. Reproduced on
# macOS 2026-09-20; filed twice, a month apart, as backlog 536 and 775 -- and
# CLAUDE.md already records a session concluding that ADR 0011's
# build-it-standalone route was broken when its shell was missing one variable.
#
# This is the only place that can say so. The toolchain file is read inside
# `project()`, before any CMake of ours runs, so no amount of machine config
# can supply the value -- which is exactly why `VCPKG_ROOT` is the one machine
# input this file cannot hold. What it CAN do is run in the prologue, before
# `project()`, and name the cause while the reader is still looking at it.
#
# Deliberately narrow. It fires only when a toolchain file was named, does not
# exist, and is a vcpkg one -- so a product configured with another toolchain,
# or with none, is untouched, and a genuine vcpkg path that resolves is never
# examined.
function(scada_check_vcpkg_toolchain)
  if(NOT CMAKE_TOOLCHAIN_FILE OR EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    return()
  endif()
  if(NOT CMAKE_TOOLCHAIN_FILE MATCHES "scripts/buildsystems/vcpkg\\.cmake$")
    return()
  endif()

  if("$ENV{VCPKG_ROOT}" STREQUAL "")
    message(FATAL_ERROR
      "VCPKG_ROOT is not set, so this product's preset asked for the vcpkg "
      "toolchain at '${CMAKE_TOOLCHAIN_FILE}' -- the preset's "
      "\$env{VCPKG_ROOT} interpolated to nothing.\n"
      "Export it before configuring:\n"
      "    export VCPKG_ROOT=/path/to/vcpkg\n"
      "It cannot come from .scada-local.cmake: the toolchain file is read "
      "inside project(), before that file is included. Delete this build "
      "directory before retrying -- CMake has already written a cache here "
      "that a later run would reuse.")
  endif()

  message(FATAL_ERROR
    "VCPKG_ROOT is set to '$ENV{VCPKG_ROOT}', but the vcpkg toolchain it "
    "names does not exist:\n"
    "    ${CMAKE_TOOLCHAIN_FILE}\n"
    "Point VCPKG_ROOT at a vcpkg checkout. Delete this build directory "
    "before retrying -- CMake has already written a cache here that a later "
    "run would reuse.")
endfunction()

# Reads the local file, if there is one. Included at directory scope from
# `scada_product_base()` so it can set cache variables, compiler launchers and
# the MSVC search paths below.
macro(scada_include_local_config)
  set(_scada_local_config "")
  if(SCADA_LOCAL_CONFIG)
    set(_scada_local_config "${SCADA_LOCAL_CONFIG}")
  elseif(DEFINED ENV{SCADA_LOCAL_CONFIG})
    set(_scada_local_config "$ENV{SCADA_LOCAL_CONFIG}")
  elseif(EXISTS "${SCADA_PRODUCT_SEARCH_ROOT}/.scada-local.cmake")
    set(_scada_local_config "${SCADA_PRODUCT_SEARCH_ROOT}/.scada-local.cmake")
  endif()

  if(_scada_local_config)
    if(NOT EXISTS "${_scada_local_config}")
      message(FATAL_ERROR
        "SCADA_LOCAL_CONFIG points at '${_scada_local_config}', which does not "
        "exist.")
    endif()
    message(STATUS "Local build config: ${_scada_local_config}")
    include("${_scada_local_config}")
  endif()
  unset(_scada_local_config)
endmacro()

# Puts the MSVC toolchain's include and library directories on the compiler and
# linker COMMAND LINE, rather than in the environment.
#
# This is the part of ADR 0011 that replaces preset `environment` blocks. The
# old arrangement worked only because a configure preset carried `INCLUDE`,
# `LIB`, `LIBPATH` and `PATH` and `inheritConfigureEnvironment` handed them to
# the build subprocess — which is why invoking the build any other way died with
# `C1083: Cannot open include file: 'type_traits'`. A CMake-time local file
# cannot set a build-time environment, so the paths travel as flags instead and
# survive any invocation.
#
# `/external:I` rather than `/I`: the toolchain headers are then treated as
# external and their warnings do not trip the tree's `/WX`.
function(scada_apply_msvc_search_paths)
  if(NOT MSVC)
    return()
  endif()
  foreach(_dir IN LISTS SCADA_MSVC_INCLUDE_DIRS)
    add_compile_options("SHELL:/external:I \"${_dir}\"")
  endforeach()
  if(SCADA_MSVC_INCLUDE_DIRS)
    add_compile_options(/external:W0)
  endif()
  foreach(_dir IN LISTS SCADA_MSVC_LIB_DIRS)
    add_link_options("/LIBPATH:${_dir}")
  endforeach()
endfunction()

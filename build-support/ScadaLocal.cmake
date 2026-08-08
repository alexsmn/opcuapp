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

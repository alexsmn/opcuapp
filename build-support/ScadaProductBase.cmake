# The build settings a product needs, wherever it is built from.
#
# These used to come from the superproject's root `CMakeLists.txt`, which meant
# a product built on its own silently lost them and a product built inside
# another silently inherited the CONSUMER's — `third_party/net` compiled at
# `core`'s C++ standard and was cppchecked against `core`'s suppressions file, a
# file it does not have. Owning them per product fixes both halves.
#
# Two calls, and the order matters:
#
#   scada_product_prologue()   BEFORE project()
#   scada_product_base()       immediately after project()
#
# The split is forced by the vcpkg toolchain, which runs inside `project()` and
# reads `VCPKG_TARGET_TRIPLET`, `VCPKG_INSTALLED_DIR` and the manifest variables
# at that moment. A machine config included after `project()` would be read too
# late to set any of them, so the prologue exists to get `.scada-local.cmake` in
# before the toolchain. Anything that needs to know the compiler — MSVC search
# paths, cppcheck — has to wait for the base call.
#
# The base settings split in two as well:
#
#   always            the product's own compilation contract — C++ standard,
#                     platform definitions. These must hold however the product
#                     is built, including spliced into a consumer.
#   top-level only    whole-build policy — warnings-as-errors, output layout,
#                     linker flags. A consumed product must not impose these on
#                     the consumer that added it.
#
# See docs/adr/0011-standalone-product-builds.md.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/ScadaLocal.cmake")

# Before `project()`. Reads the machine config, but only for the product being
# built directly: when this product has been spliced into a consumer, the
# consumer already read it, and re-reading would let a consumed product's
# prologue overwrite cache variables the consumer chose.
#
# `PROJECT_NAME` is the test. At this point in a top-level product no `project()`
# has run at all; in a consumed one the consumer's is already in scope.
macro(scada_product_prologue)
  if(NOT DEFINED PROJECT_NAME)
    scada_include_local_config()
  endif()
endmacro()

# `scada_product_base([CXX_STANDARD <n>])`
#
# The standard is a per-product decision, not a tree-wide one: `graph_qt` is
# C++20 and the SCADA products are C++23. Passing it here rather than letting a
# product set `CMAKE_CXX_STANDARD` after the call is what makes it hold when the
# product is spliced into a consumer that uses a different one — which is half
# of the settings leak this file exists to close.
macro(scada_product_base)
  if(NOT DEFINED PROJECT_NAME)
    message(FATAL_ERROR
      "scada_product_base() must be called after project().")
  endif()

  cmake_parse_arguments(_scada_base "" "CXX_STANDARD" "" ${ARGN})
  if(_scada_base_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "scada_product_base(): unexpected arguments "
      "'${_scada_base_UNPARSED_ARGUMENTS}'")
  endif()
  if(NOT _scada_base_CXX_STANDARD)
    set(_scada_base_CXX_STANDARD 23)
  endif()

  # --- always ---------------------------------------------------------------

  set(CMAKE_CXX_STANDARD ${_scada_base_CXX_STANDARD})
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  if(WIN32)
    add_definitions(
      # Required for `DiscardVirtualMemory`.
      -DWINBLUE_KBSPRING14
      -DWINVER=0x0603
      -D_WIN32_WINNT=0x0603
      -D_MSVC_STL_HARDENING
    )
  else()
    add_compile_options(-fPIC)
  endif()

  # --- top-level only -------------------------------------------------------

  if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    set(Boost_NO_WARN_NEW_VERSIONS ON)
    set(VCPKG_APPLOCAL_DEPS ON CACHE BOOL
        "Copy dependencies next to executables." FORCE)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)

    # One `bin/` per product build tree — NOT one shared across products.
    # ADR 0011 chose per-product output with no staging, so whoever needs a
    # binary is told its path rather than finding it in a common directory.
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

    if(WIN32)
      add_compile_options(/WX)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SAFESEH:NO")
      set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MDd /JMC /sdl")
      set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MD")
    endif()

    scada_apply_msvc_search_paths()
    scada_configure_cppcheck()
  endif()
endmacro()

# Static analysis against the product's OWN suppressions file.
#
# Opt-in (`SCADA_ENABLE_CPPCHECK`, typically set in the local config) because
# cppcheck is not installed everywhere and a customer building an export should
# not need it. What matters here is the suppressions file: whichever product is
# being built, cppcheck reads that product's own, never a consumer's.
function(scada_configure_cppcheck)
  if(NOT SCADA_ENABLE_CPPCHECK)
    return()
  endif()

  if(SCADA_CPPCHECK_EXECUTABLE)
    set(_exe "${SCADA_CPPCHECK_EXECUTABLE}")
  else()
    find_program(SCADA_CPPCHECK_PROGRAM cppcheck REQUIRED)
    set(_exe "${SCADA_CPPCHECK_PROGRAM}")
  endif()

  set(_suppressions "")
  foreach(_candidate IN ITEMS
      "${PROJECT_SOURCE_DIR}/cppcheck-suppressions.txt"
      "${PROJECT_SOURCE_DIR}/.cppcheck-suppressions")
    if(EXISTS "${_candidate}")
      set(_suppressions "${_candidate}")
      break()
    endif()
  endforeach()

  set(_cmd "${_exe}" "--enable=warning,performance,portability" "--inline-suppr"
           "--error-exitcode=1")
  if(_suppressions)
    list(APPEND _cmd "--suppressions-list=${_suppressions}")
  else()
    message(STATUS
      "cppcheck: ${PROJECT_NAME} has no suppressions file; running without one")
  endif()
  set(CMAKE_CXX_CPPCHECK "${_cmd}" PARENT_SCOPE)
endfunction()

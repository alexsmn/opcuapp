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

# Captured here because CMAKE_CURRENT_LIST_DIR names the kit only while this
# file is being read; scada_qt_import_offscreen_platform_into_tests() below
# reads it back from the global property when a product calls it.
set_property(GLOBAL PROPERTY SCADA_QT_OFFSCREEN_PLATFORM_CONFIG
  "${CMAKE_CURRENT_LIST_DIR}/qt_offscreen_platform.json")

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
    scada_apply_overlay_ports()
  endif()
endmacro()

# Puts the tree's vcpkg overlay ports on `VCPKG_OVERLAY_PORTS`, for whichever
# layout this product is being built in.
#
# The overlays in `ports/` are local overrides of upstream vcpkg ports, carried
# until upstream takes each fix (see `ports/README.md`). vcpkg finds them
# through `overlay-ports` in the `vcpkg-configuration.json` it reads from the
# MANIFEST directory -- and until ADR 0011 phase 6 there was one manifest, at
# the tree root, with the config beside it. Since phase 6 every product carries
# its own `vcpkg.json`, so every product is its own manifest root, and the
# tree-root config is no longer next to any of them. All 25 then resolved
# against unpatched upstream ports, silently: an `opentelemetry-cpp` without
# `restore-metric-reader-shutdown-lock.patch` stalls `MeterProvider` shutdown
# for a whole export interval, which presents as a ~60 s shutdown in a deployed
# tier and as a hang in `scada_metrics_unittests`.
#
# Setting the variable here rather than committing a `vcpkg-configuration.json`
# per product is what keeps ONE statement of the overlay path. It also avoids a
# path that must be written differently per layout: `SCADA_PRODUCT_SEARCH_ROOT`
# is the tree root in the monorepo and the product root in an export, and both
# layouts put `ports/` directly beneath it (the export carries it -- see
# `tools/export/products.toml`).
#
# The `IS_DIRECTORY` guard is not defensive tidiness: vcpkg rejects an overlay
# path that is not an existing directory ("Overlay path ... must be an existing
# directory"), so an unguarded value would break every configure of a tree
# without `ports/` -- which is what this one becomes the day the last overlay is
# dropped.
#
# Appended, not assigned, so a `-D` on the command line or a value from
# `.scada-local.cmake` survives. A plain variable is enough: the vcpkg toolchain
# does `set(VCPKG_OVERLAY_PORTS "${VCPKG_OVERLAY_PORTS}" CACHE STRING ... FORCE)`
# inside `project()`, which reads whatever is in scope here. Listing the same
# directory twice is harmless, and does happen -- the superproject root and
# every export still carry a `vcpkg-configuration.json` naming `./ports`.
macro(scada_apply_overlay_ports)
  if(NOT DEFINED SCADA_PRODUCT_SEARCH_ROOT)
    message(FATAL_ERROR
      "scada_product_prologue(): SCADA_PRODUCT_SEARCH_ROOT is not set. "
      "Include build-support/ScadaProducts.cmake before "
      "build-support/ScadaProductBase.cmake.")
  endif()
  if(IS_DIRECTORY "${SCADA_PRODUCT_SEARCH_ROOT}/ports")
    list(APPEND VCPKG_OVERLAY_PORTS "${SCADA_PRODUCT_SEARCH_ROOT}/ports")
    list(REMOVE_DUPLICATES VCPKG_OVERLAY_PORTS)
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

    # Genuine out-of-tree SDKs — the OPC Foundation stack, midl and the Classic
    # OPC client that common/opc and common/vidicon need on Windows. They are
    # not products of this tree, so the resolver cannot find them by name and
    # they have no place in `.scada-tree`; the machine config names where this
    # machine keeps them. This replaces the `$env{THIRD_PARTY}` indirection the
    # old presets carried.
    if(SCADA_EXTRA_MODULE_PATH)
      list(APPEND CMAKE_MODULE_PATH ${SCADA_EXTRA_MODULE_PATH})
    endif()
  endif()
endmacro()

# Static analysis against the nearest suppressions file, walking up.
#
# Opt-in (`SCADA_ENABLE_CPPCHECK`, typically set in the local config) because
# cppcheck is not installed everywhere and a customer building an export should
# not need it.
#
# Resolution is clang-format's rule, deliberately: start at the product root and
# walk up to `SCADA_PRODUCT_SEARCH_ROOT`, taking the first file found. Nearest
# wins, so a product that owns one still gets its own and never a consumer's --
# `core`, `client` and `common` do. What changed on 2026-08-26 is the other 20
# products, which own none: they used to get NO suppressions file at all, and
# now inherit the tree's. That is what this file already claimed was happening
# -- CLAUDE.md's cppcheck paragraph cites the `missingIncludeSystem` suppression
# as the reason a missing system header is tolerated, which was true only for
# the three products that carried their own copy of it.
#
# The walk is bounded by `SCADA_PRODUCT_SEARCH_ROOT` rather than running to the
# filesystem root, which is the same bound `scada_apply_overlay_ports` uses and
# the reason an export behaves identically: the search root is the tree root in
# the monorepo and the product root in an export, and `tools/export/products.toml`
# gives every product that owns no suppressions file a copy of the tree's at
# exactly that spot. A product that owns one is deliberately NOT given the
# tree's as well -- two sources mapping to one export path is resolved by
# fast-import ordering, not by anything guaranteed.
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

  # Bound the walk at the search root. Without it a product configured from
  # outside the tree would climb into the user's home directory looking for a
  # dotfile, which is exactly the accident clang-format's unbounded walk makes.
  if(DEFINED SCADA_PRODUCT_SEARCH_ROOT)
    get_filename_component(_stop "${SCADA_PRODUCT_SEARCH_ROOT}" ABSOLUTE)
  else()
    set(_stop "${PROJECT_SOURCE_DIR}")
  endif()

  set(_suppressions "")
  set(_dir "${PROJECT_SOURCE_DIR}")
  while(NOT _suppressions)
    foreach(_candidate IN ITEMS
        "${_dir}/cppcheck-suppressions.txt"
        "${_dir}/.cppcheck-suppressions")
      if(EXISTS "${_candidate}")
        set(_suppressions "${_candidate}")
        break()
      endif()
    endforeach()
    if(_suppressions OR _dir STREQUAL _stop)
      break()
    endif()
    get_filename_component(_parent "${_dir}/.." ABSOLUTE)
    # Reaching the filesystem root means PROJECT_SOURCE_DIR was never under the
    # search root, so there is nothing above left to check.
    if(_parent STREQUAL _dir)
      break()
    endif()
    set(_dir "${_parent}")
  endwhile()

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

# scada_qt_import_offscreen_platform_into_tests()
#
# Links Qt's `offscreen` platform plugin into every test executable this
# product defines, so the tests can run with `QT_QPA_PLATFORM=offscreen`.
# Call it at the END of the product's root CMakeLists.txt, after every
# add_subdirectory(), because it works on the targets that exist by then.
#
# Why a test needs it. With a static Qt only the platform plugins Qt considers
# default for the host are linked in -- cocoa on macOS, windows on Windows --
# and a process asked for any other one aborts inside QApplication's
# constructor with `Could not find the Qt platform plugin "offscreen"`. The
# client's `client_qt` and `client_screenshot_generator` each name the plugin
# for that reason; this does the same for the test binaries, which the products' test entry points default to the
# offscreen platform on macOS (see `client/aui/test/qt/app_environment.h` and
# `designer/test/gtest_main.cpp`) so that a `ctest` run stops bouncing a Dock
# icon and stealing focus once per case.
#
# Which targets. Every EXECUTABLE named `*_unittests` or `*_tests` whose source
# directory is inside PROJECT_SOURCE_DIR -- the two test-binary naming
# conventions the tree uses (`scada_orphan_unittests_check` scans the same
# two). Consumed products spliced into this build are skipped: their tests
# are theirs, and the ones this tree has are Qt-free anyway. On a Qt-free
# test binary the import is inert -- `qt_import_plugins` only records a target
# property that Qt's link-time conditions read, and those conditions are
# evaluated only when a Qt module is in the link closure -- so matching by
# name rather than by "links Qt" costs nothing and needs no transitive walk.
# With a shared Qt, `qt_import_plugins` is a no-op and the plugin loads from
# the plugin directory at runtime as it always did.
#
# Which screen. The plugin's built-in screen is 800x600, and
# `QWidget::restoreGeometry()` clamps a window to the screen it lands on, so
# a test that saves a 1000-wide window and expects it back gets 798 -- four
# Designer tests did. `qt_offscreen_platform.json` beside this file describes
# a 1920x1080 screen instead, and every imported target is told where it is
# through the `SCADA_QT_OFFSCREEN_PLATFORM_CONFIG` compile definition, which
# the entry points splice into `QT_QPA_PLATFORM=offscreen:configfile=<path>`.
# An absolute source path baked into a test binary is nothing new -- the
# goldens' directory arrives the same way -- and a test binary was never
# relocatable.
#
# The definition is also the entry points' only trigger: they force the
# offscreen platform when it is defined and leave the platform alone when it
# is not. Setting it here, on the target the plugin was just imported into,
# is what keeps the two halves from coming apart -- a build tree configured
# before this function existed compiles the header half without the link
# half, and had the header forced `offscreen` on its own, every widget test
# in that tree would have aborted at startup (measured: 345 failures in a
# peer's tree on 2026-09-07). Keep the definition and the import in this one
# loop body for that reason.
#
# Guarded the same way as the three sites above: a Qt build without the
# plugin, or a product that never found Qt, configures unchanged.
function(scada_qt_import_offscreen_platform_into_tests)
  if(NOT COMMAND qt_import_plugins OR NOT TARGET Qt6::QOffscreenIntegrationPlugin)
    return()
  endif()

  get_property(_config GLOBAL PROPERTY SCADA_QT_OFFSCREEN_PLATFORM_CONFIG)
  if(NOT EXISTS "${_config}")
    message(FATAL_ERROR
      "scada_qt_import_offscreen_platform_into_tests(): the offscreen screen "
      "configuration is missing at ${_config}")
  endif()

  set(_pending "${CMAKE_CURRENT_SOURCE_DIR}")
  set(_imported "")
  while(_pending)
    list(POP_FRONT _pending _dir)
    get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
    list(APPEND _pending ${_subdirs})

    get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
      if(NOT _target MATCHES "_unittests$|_tests$")
        continue()
      endif()
      get_target_property(_type "${_target}" TYPE)
      if(NOT _type STREQUAL "EXECUTABLE")
        continue()
      endif()
      get_target_property(_source_dir "${_target}" SOURCE_DIR)
      if(NOT _source_dir STREQUAL "${PROJECT_SOURCE_DIR}"
         AND NOT _source_dir MATCHES "^${PROJECT_SOURCE_DIR}/")
        continue()
      endif()
      qt_import_plugins("${_target}" INCLUDE Qt6::QOffscreenIntegrationPlugin)
      target_compile_definitions("${_target}" PRIVATE
        "SCADA_QT_OFFSCREEN_PLATFORM_CONFIG=\"${_config}\"")
      list(APPEND _imported "${_target}")
    endforeach()
  endwhile()

  list(LENGTH _imported _count)
  message(STATUS
    "${PROJECT_NAME}: offscreen platform plugin imported into ${_count} "
    "test executables")
endfunction()

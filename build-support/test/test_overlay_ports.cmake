# Does `scada_product_prologue()` put the tree's vcpkg overlay ports where the
# vcpkg toolchain will read them?
#
# Run with `cmake -P`. Source-only: it copies the kit into synthesized trees
# under a scratch directory and includes it there, so it exercises the real
# `build-support/` files without a compiler, a toolchain or a vcpkg.
#
# Worth a test of its own because the regression it guards was invisible. ADR
# 0011 phase 6 gave every product its own `vcpkg.json`, which made every product
# its own vcpkg manifest root and left the tree-root `vcpkg-configuration.json`
# beside none of them. Nothing failed: the products configured, built and
# passed, against unpatched upstream ports. It surfaced only as an
# `opentelemetry-cpp` missing its shutdown fix -- a stall, not an error -- and
# was found by reading `vcpkg_abi_info.txt` by hand.
#
# The layout cases are the two a product is ever built in (ADR 0011): the
# monorepo, where `build-support/` and `ports/` sit at the tree root, and an
# export, where both are dropped into the product root. The kit derives the
# search root from its own location in both, so the cases differ only in what
# the tree around it looks like.

cmake_minimum_required(VERSION 3.20)

get_filename_component(_kit "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT SCADA_TEST_SCRATCH_DIR)
  set(SCADA_TEST_SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/overlay-ports-test")
endif()
file(REMOVE_RECURSE "${SCADA_TEST_SCRATCH_DIR}")

# Synthesizes one tree and runs the prologue in it, leaving `CASE_ROOT` and the
# resulting `VCPKG_OVERLAY_PORTS` in the caller's scope.
#
# A macro rather than a function so the kit's variables -- `SCADA_PRODUCT_SEARCH_ROOT`
# above all -- land where the cases below can go on calling the prologue
# directly. Each case gets its own copy of the kit: `include_guard()` keys on
# the file path, so distinct copies re-run, while one shared copy would be
# included once and every case after the first would silently assert about the
# first case's state.
macro(run_case name with_ports preset)
  set(CASE_ROOT "${SCADA_TEST_SCRATCH_DIR}/${name}")
  file(COPY "${_kit}/ScadaProducts.cmake" "${_kit}/ScadaProductBase.cmake"
            "${_kit}/ScadaLocal.cmake"
       DESTINATION "${CASE_ROOT}/build-support")
  if(${with_ports})
    file(MAKE_DIRECTORY "${CASE_ROOT}/ports/some-port")
  endif()

  set(VCPKG_OVERLAY_PORTS "${preset}")
  include("${CASE_ROOT}/build-support/ScadaProducts.cmake")
  include("${CASE_ROOT}/build-support/ScadaProductBase.cmake")
  scada_product_prologue()
endmacro()

macro(expect_overlay what expected)
  if("${VCPKG_OVERLAY_PORTS}" STREQUAL "${expected}")
    message(STATUS "ok: ${what} -> '${VCPKG_OVERLAY_PORTS}'")
  else()
    set(SCADA_TEST_FAILED TRUE)
    message(SEND_ERROR
      "${what}: expected '${expected}', got '${VCPKG_OVERLAY_PORTS}'")
  endif()
endmacro()

# --- monorepo layout: ports/ beside build-support/ at the tree root ----------

run_case(monorepo TRUE "")
expect_overlay("monorepo layout" "${CASE_ROOT}/ports")

# --- export layout: the same shape, rooted at the product --------------------
#
# An export drops `build-support/` and `ports/` into the product root
# (tools/export/products.toml), so the kit's own search root is the product's.
# Asserted separately from the monorepo case because the two are separate
# promises to separate audiences, and a change that special-cased one layout
# would otherwise pass.

run_case(export TRUE "")
expect_overlay("export layout" "${CASE_ROOT}/ports")

# --- no ports/ at all --------------------------------------------------------
#
# vcpkg rejects an overlay path that is not an existing directory, so naming one
# unconditionally would break every configure of a tree without `ports/` -- the
# state this tree is meant to reach once upstream has taken the last fix and the
# whole directory is deleted (ports/README.md).

run_case(no_ports FALSE "")
expect_overlay("no ports/" "")

# --- an existing value is kept -----------------------------------------------
#
# A `-D` on the command line and `.scada-local.cmake` both reach this variable;
# the tree's overlay is added to what they chose rather than replacing it.

run_case(preset_kept TRUE "/somewhere/else")
expect_overlay("existing value kept" "/somewhere/else;${CASE_ROOT}/ports")

# --- the tree's own path is not added twice ----------------------------------
#
# The variable is a cache entry the vcpkg toolchain FORCEs, so a reconfigure
# re-enters the prologue with the previous run's value already in hand.

run_case(reconfigure TRUE "")
scada_product_prologue()
expect_overlay("reconfigure" "${CASE_ROOT}/ports")

# --- a consumed product does not touch the consumer's choice -----------------
#
# When a product is spliced into another with `add_subdirectory`, its prologue
# runs with the consumer's `project()` already in scope. The consumer's vcpkg
# install has been planned by then, so re-deriving the overlay there could only
# disagree with what was actually used.

set(PROJECT_NAME "the-consumer")
set(VCPKG_OVERLAY_PORTS "")
scada_product_prologue()
expect_overlay("consumed product" "")
unset(PROJECT_NAME)

file(REMOVE_RECURSE "${SCADA_TEST_SCRATCH_DIR}")
if(NOT SCADA_TEST_FAILED)
  message(STATUS "overlay ports: all cases passed")
endif()

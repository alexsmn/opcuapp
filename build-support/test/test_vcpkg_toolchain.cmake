# Does `scada_product_prologue()` name VCPKG_ROOT when the preset's
# `$env{VCPKG_ROOT}` interpolated to nothing?
#
# Run with `cmake -P`. Source-only: it copies the kit into a scratch directory
# and runs the real prologue there, so it needs no compiler, no toolchain and
# no vcpkg.
#
# Worth a test of its own because the failure it guards is one a reader cannot
# diagnose from CMake's own output, and because it was filed twice a month
# apart before anything was done about it (backlog 536, 2026-08-26; backlog
# 775, 2026-09-19). Every product's `ninja` preset names
# `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake` as its toolchain file,
# CMake interpolates an unset variable to the empty string rather than
# complaining, and the result is three errors -- a missing toolchain at
# `/scripts/buildsystems/vcpkg.cmake`, no CMAKE_MAKE_PROGRAM, no
# CMAKE_CXX_COMPILER -- none of which mentions the variable, plus a stub
# CMakeCache.txt that reads as a configured tree.
#
# Each case runs in its OWN `cmake -P` process, because what is being asserted
# is a `message(FATAL_ERROR)` and a script cannot survive its own. That is also
# what lets the environment differ per case: `cmake -E env --unset=` is the
# portable way to run a child without a variable the parent has.

cmake_minimum_required(VERSION 3.20)

get_filename_component(_kit "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT SCADA_TEST_SCRATCH_DIR)
  set(SCADA_TEST_SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/vcpkg-toolchain-test")
endif()
file(REMOVE_RECURSE "${SCADA_TEST_SCRATCH_DIR}")

# The kit, plus the one-line script each case runs against it.
set(CASE_ROOT "${SCADA_TEST_SCRATCH_DIR}/tree")
file(COPY "${_kit}/ScadaProducts.cmake" "${_kit}/ScadaProductBase.cmake"
          "${_kit}/ScadaLocal.cmake"
     DESTINATION "${CASE_ROOT}/build-support")

# The prologue rather than the check alone: a check nothing calls would pass
# every case here and fail every real configure.
file(WRITE "${CASE_ROOT}/case.cmake"
  "include(\"${CASE_ROOT}/build-support/ScadaProducts.cmake\")\n"
  "include(\"${CASE_ROOT}/build-support/ScadaProductBase.cmake\")\n"
  "scada_product_prologue()\n"
  "message(STATUS \"prologue completed\")\n")

# A vcpkg that exists, for the cases about a toolchain that resolves.
set(REAL_VCPKG "${SCADA_TEST_SCRATCH_DIR}/vcpkg")
file(WRITE "${REAL_VCPKG}/scripts/buildsystems/vcpkg.cmake"
     "# stand-in for the vcpkg toolchain; never included by these cases\n")

# Runs one case and leaves `CASE_RESULT` and `CASE_OUTPUT` in the caller's
# scope. `env_args` is what to hand `cmake -E env` -- an assignment, or
# `--unset=NAME`.
macro(run_case what env_args toolchain)
  set(_toolchain_arg)
  if(NOT "${toolchain}" STREQUAL "")
    set(_toolchain_arg "-DCMAKE_TOOLCHAIN_FILE=${toolchain}")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env ${env_args}
            ${CMAKE_COMMAND} ${_toolchain_arg} -P "${CASE_ROOT}/case.cmake"
    RESULT_VARIABLE CASE_RESULT
    OUTPUT_VARIABLE CASE_OUTPUT
    ERROR_VARIABLE CASE_ERROR)
  string(APPEND CASE_OUTPUT "${CASE_ERROR}")
endmacro()

macro(expect_refused what needle)
  if(CASE_RESULT EQUAL 0)
    set(SCADA_TEST_FAILED TRUE)
    message(SEND_ERROR "${what}: expected a refusal, the prologue completed")
  elseif(NOT CASE_OUTPUT MATCHES "${needle}")
    set(SCADA_TEST_FAILED TRUE)
    message(SEND_ERROR
      "${what}: refused, but the message does not mention '${needle}':\n${CASE_OUTPUT}")
  else()
    message(STATUS "ok: ${what}")
  endif()
endmacro()

macro(expect_allowed what)
  if(CASE_RESULT EQUAL 0)
    message(STATUS "ok: ${what}")
  else()
    set(SCADA_TEST_FAILED TRUE)
    message(SEND_ERROR "${what}: expected the prologue to complete:\n${CASE_OUTPUT}")
  endif()
endmacro()

# --- the reported failure: VCPKG_ROOT unset, so the preset named a bare path -
#
# The message has to carry the variable's NAME, which is the whole point: the
# three errors CMake produces on its own never mention it.

run_case("unset" "--unset=VCPKG_ROOT" "/scripts/buildsystems/vcpkg.cmake")
expect_refused("VCPKG_ROOT unset" "VCPKG_ROOT is not set")

# --- set, but pointing at no vcpkg -------------------------------------------
#
# A different mistake with the same symptom, so it gets a message of its own
# that quotes the value rather than telling the reader to set what is set.

run_case("wrong" "VCPKG_ROOT=/nonexistent/vcpkg"
         "/nonexistent/vcpkg/scripts/buildsystems/vcpkg.cmake")
expect_refused("VCPKG_ROOT pointing nowhere" "VCPKG_ROOT is set to")

# --- the ordinary case: it resolves, and the prologue runs to the end --------
#
# This is the case that matters most for anything downstream. The check sits
# at the TOP of the prologue, ahead of the local config and the overlay-ports
# append, and a product may depend on reaching the region after it -- `display`
# appends to VCPKG_OVERLAY_TRIPLETS between the prologue and `project()` and
# resolves a plain Qt if it does not. So assert the prologue COMPLETES rather
# than merely that it did not refuse.

run_case("resolves" "VCPKG_ROOT=${REAL_VCPKG}"
         "${REAL_VCPKG}/scripts/buildsystems/vcpkg.cmake")
expect_allowed("a vcpkg toolchain that resolves")
if(NOT CASE_OUTPUT MATCHES "prologue completed")
  set(SCADA_TEST_FAILED TRUE)
  message(SEND_ERROR
    "a vcpkg toolchain that resolves: the prologue returned 0 without "
    "reaching its end:\n${CASE_OUTPUT}")
endif()

# --- no toolchain file named at all ------------------------------------------
#
# A product configured without vcpkg is none of this check's business, and a
# check that refused here would break every such configure.

run_case("none" "--unset=VCPKG_ROOT" "")
expect_allowed("no toolchain file named")

# --- some other toolchain, and it does not exist -----------------------------
#
# Also not this check's business: the reader named that path themselves, and
# CMake's own "Could not find toolchain file" says everything there is to say
# about it. Asserted so the match stays anchored on the vcpkg path shape
# rather than drifting into "any missing toolchain".

run_case("foreign" "--unset=VCPKG_ROOT" "/nonexistent/some-other-toolchain.cmake")
expect_allowed("a non-vcpkg toolchain that does not exist")

if(SCADA_TEST_FAILED)
  message(FATAL_ERROR "vcpkg toolchain check: cases failed")
endif()
message(STATUS "vcpkg toolchain check: all cases passed")

# Does scada_skip_linting_begin()/end() keep a bracketed product's sources
# away from the linter, on every CMake version the tree supports?
#
# Run with `cmake -P`, handing it a C++ compiler's generator:
#   cmake -DCMAKE_MAKE_PROGRAM=<ninja> -P test_skip_linting.cmake
#
# It BUILDS a three-directory project -- top, a consumed product `dep`, and a
# vendor tree `dep` itself brackets -- with a stand-in linter that records
# every source it is handed. What the linter saw is the only evidence worth
# having: a target property can say "skip" while the linter runs anyway, which
# is exactly what `set(CMAKE_SKIP_LINTING ON)` did before CMake 4.2, where the
# variable does not exist.
#
# Three cases, because a fix test that cannot fail proves nothing:
#
#   raw     the idiom the call sites used to carry. Must reproduce the defect
#           on every version: `dep`'s own source is linted, because `dep`'s
#           `set(CMAKE_SKIP_LINTING OFF)` switches linting back on inside a
#           product that is itself being skipped.
#   cache   the helper, with the linter given as a cache entry (`-D`), so the
#           restore has to uncover it again for `top`.
#   normal  the helper, with the linter set as a normal variable, the way
#           scada_configure_cppcheck() sets it.

cmake_minimum_required(VERSION 3.25)

get_filename_component(_kit "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT SCADA_TEST_SCRATCH_DIR)
  set(SCADA_TEST_SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/skip-linting-test")
endif()
file(REMOVE_RECURSE "${SCADA_TEST_SCRATCH_DIR}")

# --- the stand-in linter ------------------------------------------------------

# CMake appends the compile arguments and the source to the linter command, so
# the script finds the source as the argument that names an existing *.cpp.
set(_linter "${SCADA_TEST_SCRATCH_DIR}/lint.cmake")
file(WRITE "${_linter}" [=[
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE 1 ${_last})
  if(CMAKE_ARGV${_i} MATCHES "\\.cpp$")
    get_filename_component(_name "${CMAKE_ARGV${_i}}" NAME)
    file(APPEND "${LOG}" "${_name}\n")
  endif()
endforeach()
]=])

# --- the project --------------------------------------------------------------

macro(write_project name begin end)
  set(_src "${SCADA_TEST_SCRATCH_DIR}/src-${name}")
  file(COPY "${_kit}/ScadaProducts.cmake" "${_kit}/ScadaProductBase.cmake"
            "${_kit}/ScadaLocal.cmake"
       DESTINATION "${_src}/build-support")
  file(WRITE "${_src}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.25)
project(skip_linting LANGUAGES CXX)
include(build-support/ScadaProductBase.cmake)
if(DEFINED LINT_AS_NORMAL)
  set(CMAKE_CXX_CPPCHECK \${LINT_AS_NORMAL})
endif()
${begin}
add_subdirectory(dep)
${end}
add_library(top_lib STATIC top.cpp)
")
  file(WRITE "${_src}/dep/CMakeLists.txt" "
${begin}
add_subdirectory(vendor)
${end}
add_library(dep_lib STATIC dep.cpp)
")
  file(WRITE "${_src}/dep/vendor/CMakeLists.txt"
    "add_library(vendor_lib STATIC vendor.cpp)\n")
  file(WRITE "${_src}/top.cpp" "int top() { return 1; }\n")
  file(WRITE "${_src}/dep/dep.cpp" "int dep() { return 2; }\n")
  file(WRITE "${_src}/dep/vendor/vendor.cpp" "int vendor() { return 3; }\n")
endmacro()

write_project(raw "set(CMAKE_SKIP_LINTING ON)" "set(CMAKE_SKIP_LINTING OFF)")
write_project(helper "scada_skip_linting_begin()" "scada_skip_linting_end()")

# Configures and builds `project` as case `name`; `how` is CACHE or NORMAL.
macro(build_case name project how)
  set(_bin "${SCADA_TEST_SCRATCH_DIR}/build-${name}")
  set(_log "${SCADA_TEST_SCRATCH_DIR}/lint-${name}.log")
  file(WRITE "${_log}" "")
  set(_lint_cmd "${CMAKE_COMMAND};-DLOG=${_log};-P;${_linter}")
  if("${how}" STREQUAL "CACHE")
    set(_lint_arg "-DCMAKE_CXX_CPPCHECK=${_lint_cmd}")
  else()
    set(_lint_arg "-DLINT_AS_NORMAL=${_lint_cmd}")
  endif()
  set(_make_program)
  if(CMAKE_MAKE_PROGRAM)
    set(_make_program "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -S "${SCADA_TEST_SCRATCH_DIR}/src-${project}"
            -B "${_bin}" -G Ninja ${_make_program} "${_lint_arg}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${name}: configure failed:\n${_out}")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${_bin}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${name}: build failed:\n${_out}")
  endif()
  file(STRINGS "${_log}" _linted)
endmacro()

# Asserts whether `source` was handed to the linter in the last case built.
macro(expect_linted name source expected)
  if("${source}" IN_LIST _linted)
    set(_was TRUE)
  else()
    set(_was FALSE)
  endif()
  if(_was STREQUAL "${expected}")
    message(STATUS "ok: ${name}: ${source} linted=${_was}")
  else()
    set(SCADA_TEST_FAILED TRUE)
    message(SEND_ERROR
      "${name}: ${source} linted=${_was}, expected ${expected} "
      "(linted: ${_linted})")
  endif()
endmacro()

# --- the cases ----------------------------------------------------------------

build_case(raw raw CACHE)
# The defect, reproduced on every version.
expect_linted(raw top.cpp TRUE)
expect_linted(raw dep.cpp TRUE)

foreach(_how IN ITEMS CACHE NORMAL)
  string(TOLOWER "${_how}" _name)
  build_case(${_name} helper ${_how})
  expect_linted(${_name} top.cpp TRUE)
  expect_linted(${_name} dep.cpp FALSE)
  expect_linted(${_name} vendor.cpp FALSE)
endforeach()

if(SCADA_TEST_FAILED)
  message(FATAL_ERROR "scada_skip_linting_begin()/end() is broken on CMake "
                      "${CMAKE_VERSION}")
endif()

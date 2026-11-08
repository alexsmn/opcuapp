# Does scada_product_base() make -Wnon-virtual-dtor an error on the host's
# compiler, Clang and GCC alike (backlog 725)?
#
# Run with `cmake -P`, handing it a C++ compiler's generator:
#   cmake -DCMAKE_MAKE_PROGRAM=<ninja> -P test_non_virtual_dtor.cmake
#
# It BUILDS a two-directory project -- a top level and a product spliced into
# it that calls scada_product_base() -- because the flag lives in the
# always-applied block and so must reach a product built inside a consumer,
# where no top-level -Werror applies. Two cases, so the test cannot pass by
# rejecting everything:
#
#   bad    a class with virtual functions and a public non-virtual destructor.
#          The build must FAIL, naming non-virtual-dtor.
#   good   the shapes the tree relies on staying legal: a final class, and a
#          protected non-virtual destructor (MainWindowInterface,
#          OpenedViewInterface). The build must succeed.
#
# MSVC has no such switch; the test reports that and passes there.

cmake_minimum_required(VERSION 3.25)

get_filename_component(_kit "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT SCADA_TEST_SCRATCH_DIR)
  set(SCADA_TEST_SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/non-virtual-dtor-test")
endif()
file(REMOVE_RECURSE "${SCADA_TEST_SCRATCH_DIR}")

macro(write_project name body)
  set(_src "${SCADA_TEST_SCRATCH_DIR}/src-${name}")
  file(COPY "${_kit}/ScadaProducts.cmake" "${_kit}/ScadaProductBase.cmake"
            "${_kit}/ScadaLocal.cmake"
       DESTINATION "${_src}/prod/build-support")
  file(WRITE "${_src}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.25)
project(top LANGUAGES CXX)
add_subdirectory(prod)
")
  file(WRITE "${_src}/prod/CMakeLists.txt" "
project(prod LANGUAGES CXX)
include(build-support/ScadaProductBase.cmake)
scada_product_base(CXX_STANDARD 17)
file(WRITE \"\${CMAKE_BINARY_DIR}/compiler.txt\" \"\${CMAKE_CXX_COMPILER_ID}\")
add_library(prod_lib STATIC prod.cpp)
")
  file(WRITE "${_src}/prod/prod.cpp" "${body}")
endmacro()

write_project(bad [=[
struct Polymorphic {
  virtual int f() { return 1; }
};
int use() { return Polymorphic{}.f(); }
]=])

write_project(good [=[
struct Sealed final {
  virtual int f() { return 1; }
};
class Interface {
 public:
  virtual int g() = 0;
 protected:
  ~Interface() = default;
};
class Impl final : public Interface {
 public:
  int g() override { return 2; }
};
int use() { return Sealed{}.f() + Impl{}.g(); }
]=])

# Configures and builds case `name`; leaves the result in _result/_out.
macro(build_case name)
  set(_bin "${SCADA_TEST_SCRATCH_DIR}/build-${name}")
  set(_make_program)
  if(CMAKE_MAKE_PROGRAM)
    set(_make_program "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -S "${SCADA_TEST_SCRATCH_DIR}/src-${name}"
            -B "${_bin}" -G Ninja ${_make_program}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${name}: configure failed:\n${_out}")
  endif()
  file(READ "${_bin}/compiler.txt" _compiler)
  execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${_bin}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
endmacro()

build_case(good)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "good (${_compiler}): build failed, but final classes "
    "and protected destructors must stay legal:\n${_out}")
endif()
message(STATUS "ok: good (${_compiler}) builds")

if(NOT _compiler MATCHES "Clang|GNU" OR CMAKE_HOST_WIN32)
  message(STATUS "skip: bad -- ${_compiler} has no -Wnon-virtual-dtor")
  return()
endif()

build_case(bad)
if(_result EQUAL 0)
  message(FATAL_ERROR "bad (${_compiler}): a public non-virtual destructor "
    "on a polymorphic class built clean; scada_product_base() is not making "
    "-Wnon-virtual-dtor an error for this compiler:\n${_out}")
endif()
if(NOT _out MATCHES "non-virtual-dtor")
  message(FATAL_ERROR "bad (${_compiler}): the build failed, but not on "
    "non-virtual-dtor:\n${_out}")
endif()
message(STATUS "ok: bad (${_compiler}) fails on non-virtual-dtor")

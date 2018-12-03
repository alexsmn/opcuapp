cmake_minimum_required(VERSION 2.6)

project(opcuapp C CXX)

set(OPCUA_DIR ".")
set(OPCUAPP_DIR ".")

find_package(OPCUAPP REQUIRED)

add_subdirectory(samples)

# Unit-Tests

find_package(GTest)

if(GTEST_FOUND)
  enable_testing()
  file(GLOB_RECURSE unittests "tests/*.*")
  add_executable(tests ${unittests})
  target_link_libraries(tests OPCUAPP GTest::Main)
  add_test(NAME tests COMMAND tests)
endif(GTEST_FOUND)

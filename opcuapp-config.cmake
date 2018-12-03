project(OPCUAPP)

find_package(OPCUA REQUIRED)

file(GLOB_RECURSE sources
  "${CMAKE_CURRENT_LIST_DIR}/opcuapp/*.h"
)

add_library(OPCUAPP INTERFACE)

target_sources(OPCUAPP INTERFACE ${sources})

target_include_directories(OPCUAPP INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(OPCUAPP INTERFACE OPCUA)

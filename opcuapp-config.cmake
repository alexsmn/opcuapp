find_package(OPCUA REQUIRED)

file(GLOB_RECURSE SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/opcuapp/*.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/opcuapp/*.h"
)
file(GLOB_RECURSE UNITTESTS
  "${CMAKE_CURRENT_LIST_DIR}/opcuapp/*_unittest.*"
)
list(REMOVE_ITEM SOURCES ${UNITTESTS})

add_library(OPCUAPP STATIC ${SOURCES})
target_link_libraries(OPCUAPP OPCUA)
target_include_directories(OPCUAPP PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
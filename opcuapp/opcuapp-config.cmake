find_package(OPCUA REQUIRED)

file(GLOB_RECURSE SOURCES "*.cpp" "*.h")
file(GLOB_RECURSE UNITTESTS "*_unittest.*")
list(REMOVE_ITEM SOURCES ${UNITTESTS})

add_library(OPCUAPP STATIC ${SOURCES})
target_link_libraries(OPCUAPP OPCUA)
target_include_directories(OPCUAPP PUBLIC ".")
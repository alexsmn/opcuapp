file(GLOB SOURCES "*.cpp" "*.h")
add_executable(client ${SOURCES})
target_link_libraries(client OPCUAPP)

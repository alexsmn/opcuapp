file(GLOB SOURCES "*.cpp" "*.h")
add_executable(server ${SOURCES})
target_link_libraries(server OPCUAPP)

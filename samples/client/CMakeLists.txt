file(GLOB SOURCES "*.cpp" "*.h")
add_executable(client ${SOURCES})
target_link_libraries(client OPCUAPP)

if(WIN32)
  target_compile_options(server PRIVATE /permissive-)
endif()

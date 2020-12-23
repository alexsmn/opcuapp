file(GLOB SOURCES "*.cpp" "*.h")
add_executable(server ${SOURCES})
target_link_libraries(server OPCUAPP)

if(WIN32)
  target_compile_options(server PRIVATE /permissive-)
endif()

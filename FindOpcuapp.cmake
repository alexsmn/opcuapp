# FindOpcuapp.cmake — target-guarded add_subdirectory wrapper, mirroring the
# pattern used by third_party/net/FindTransport.cmake. Put this directory on
# CMAKE_MODULE_PATH and call `find_package(opcuapp REQUIRED)`; link against
# `opcuapp::opcuapp`.
if(NOT TARGET opcuapp)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR} opcuapp)
endif()

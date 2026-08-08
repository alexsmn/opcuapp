# Findopcuapp.cmake — target-guarded add_subdirectory wrapper, mirroring the
# pattern used by third_party/net/FindTransport.cmake. Put this directory on
# CMAKE_MODULE_PATH and call `find_package(opcuapp REQUIRED)`; link against
# `opcuapp::opcuapp`.
#
# The filename is lowercase deliberately, and must stay that way. CMake builds
# the module filename from the name as written at the call site, so
# `find_package(opcuapp)` opens `Findopcuapp.cmake` and nothing else. This file
# was `FindOpcuapp.cmake` until 2026-08-08: every one of the seven call sites
# spells it lowercase, so on a case-sensitive filesystem none of them resolved
# — module mode missed, config mode missed, and `REQUIRED` failed the
# configure. It never showed up because everywhere this tree builds is
# case-insensitive, the GCP Linux cross-build included (its container
# bind-mounts the source from a macOS host). See ADR 0009 and ADR 0010.
if(NOT TARGET opcuapp)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR} opcuapp)
endif()

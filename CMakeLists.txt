cmake_minimum_required(VERSION 2.6)

project(opcuapp)

set(OPCUA_DIR "${CMAKE_SOURCE_DIR}/opcuapp")
set(OPCUAPP_DIR "${CMAKE_SOURCE_DIR}/opcuapp")

find_package(opcuapp)

add_subdirectory(samples)

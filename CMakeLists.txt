cmake_minimum_required(VERSION 2.6)

project(opcuapp)

set(OPCUA_DIR ".")
set(OPCUAPP_DIR ".")

find_package(OPCUAPP REQUIRED)

add_subdirectory(samples)

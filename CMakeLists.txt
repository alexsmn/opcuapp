cmake_minimum_required(VERSION 2.6)

project(opcuapp)

set(OPCUA_DIR "opcua")
set(OPCUAPP_DIR "opcuapp")

find_package(OPCUAPP REQUIRED)

add_subdirectory(samples)

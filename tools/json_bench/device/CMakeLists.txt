cmake_minimum_required(VERSION 3.16)
# Compile only this fixture's dependency graph. No Wi-Fi/LCD demo dependency
# should distort the isolated JSON link-cost comparison.
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(NaWServer_json_bench)

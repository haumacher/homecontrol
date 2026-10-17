# Copyright (c) 2026 Bernhard Haumacher
#
# The ESP-IDF project. Each target builds in its own directory:
#   idf.py -B build-esp32c6 set-target esp32c6 && idf.py -B build-esp32c6 build

cmake_minimum_required(VERSION 3.16)

# Keep the generated sdkconfig inside the build directory, so that build-esp32c6 and
# build-esp32s3 do not overwrite each other's configuration. Settings belong in sdkconfig.defaults.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(homecontrol)

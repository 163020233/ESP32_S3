# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# NOTE: This project builds an ESP32-S3 + W5500 Ethernet validation program.
# The project name is kept as "hello_world" to stay compatible with the
# existing CLion configuration; it can be renamed freely later.
# MINIMAL_BUILD trims the build to the dependency graph only. It is required
# here because this ESP-IDF checkout has not fetched the "unity" submodule
# (components/unity/unity), which a full build would otherwise process.
idf_build_set_property(MINIMAL_BUILD ON)
project(hello_world)

# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
# set(PROJECT_VER 2.1.0.2")

set(EXTRA_COMPONENT_DIRS ./components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_minimal_ota)

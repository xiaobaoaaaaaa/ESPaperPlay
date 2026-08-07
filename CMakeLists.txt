# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
# ESPaperPlay - ESP32-S3 e-paper device platform
# See README.md for build instructions.

cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 分层组件目录：这些容器目录下的直接子目录（含 CMakeLists.txt）会被识别为组件。
# board 仍位于 components/ 下，由默认扫描直接发现。
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/drivers"
    "${CMAKE_CURRENT_LIST_DIR}/components/services"
    "${CMAKE_CURRENT_LIST_DIR}/components/graphics"
    "${CMAKE_CURRENT_LIST_DIR}/components/applications"
)

project(ESPaperPlay)

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

# 版本号唯一来源：components/board/include/espaperplay_config.h 中的
# ESPAPERPLAY_VERSION_{MAJOR,MINOR,PATCH}。此处解析并设为 PROJECT_VER，
# 使 ESP-IDF 应用描述符（esp_app_desc）版本与固件内版本保持一致，
# 不再依赖 git describe 派生（无 tag 时为 commit hash）。
set(_espaperplay_config_header
    "${CMAKE_CURRENT_LIST_DIR}/components/board/include/espaperplay_config.h")
file(READ "${_espaperplay_config_header}" _espaperplay_config_text)
foreach(_ver_part MAJOR MINOR PATCH)
    string(REGEX MATCH "ESPAPERPLAY_VERSION_${_ver_part}[ \t]+([0-9]+)"
           _espaperplay_ver_match "${_espaperplay_config_text}")
    if(NOT _espaperplay_ver_match)
        message(FATAL_ERROR
            "Cannot parse ESPAPERPLAY_VERSION_${_ver_part} from ${_espaperplay_config_header}")
    endif()
    set(_espaperplay_ver_${_ver_part} "${CMAKE_MATCH_1}")
endforeach()
set(PROJECT_VER
    "${_espaperplay_ver_MAJOR}.${_espaperplay_ver_MINOR}.${_espaperplay_ver_PATCH}")

project(ESPaperPlay)

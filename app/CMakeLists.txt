cmake_minimum_required(VERSION 3.16)

# 业务工程与官方源码分开存放：firmware/ 是上游 fork，只在本地引用，不往里写业务代码。
# 官方驱动组件（zectrix_board / zectrix_epd / zectrix_demo_ui / zectrix_self_test）
# 通过 EXTRA_COMPONENT_DIRS 直接复用；esp_codec_dev 是官方工程已经下载好的托管组件（v1.5.11），
# 一并指过来，避免重复下载。
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../firmware/components"
    "${CMAKE_CURRENT_LIST_DIR}/../firmware/managed_components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zectrix_calendar)

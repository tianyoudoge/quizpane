# 这份模板不参与主项目构建（根 CMakeLists.txt 不 add_subdirectory 这个目录）。
# 复制整个 providers/template/ 目录到你自己的仓库后，把下面这份 CMakeLists.txt
# 也一起复制过去，改一下 target 名字即可独立编译——它不依赖主项目里的任何
# CMake target，只需要你把 QUIZPANE_SDK_INCLUDE_DIR 指到 quizpane 仓库的
# sdk/include（或者干脆把 provider_abi.h 复制到你自己的目录，改这一行）。
#
# 依赖只有两个：
#   - provider_abi.h 的头文件路径（QUIZPANE_SDK_INCLUDE_DIR，纯头文件，不链接符号）
#   - Qt Core：本模板用 QJsonObject/QJsonDocument 处理 JSON，你也可以换成其他 JSON 库
cmake_minimum_required(VERSION 3.21)
project(quizpane_provider_template LANGUAGES CXX)

set(QUIZPANE_SDK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include"
    CACHE PATH "quizpane sdk/include 的路径（提供 quizpane/provider_abi.h）")

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

set(CMAKE_AUTOMOC ON)

add_library(quizpane_provider_template SHARED provider_template.cpp)
target_include_directories(quizpane_provider_template PRIVATE "${QUIZPANE_SDK_INCLUDE_DIR}")
target_compile_definitions(quizpane_provider_template PRIVATE QUIZPANE_PROVIDER_BUILD)
target_link_libraries(quizpane_provider_template PRIVATE Qt${QT_VERSION_MAJOR}::Core)
set_target_properties(quizpane_provider_template PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

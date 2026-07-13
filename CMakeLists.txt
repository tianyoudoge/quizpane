cmake_minimum_required(VERSION 3.24)

project(QuizPane VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)

option(QUIZPANE_BUILD_TESTS "Build QuizPane tests" ON)
option(QUIZPANE_BUILD_BANK_GENERATOR
       "Build the experimental local question-bank generator" OFF)
option(QUIZPANE_BUILD_BANK_STUDIO "Build the QuizPane Bank Studio GUI" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network)
find_package(Qt6CorePrivate ${Qt6Core_VERSION} EXACT REQUIRED)

FetchContent_Declare(qrcodegen_source
    GIT_REPOSITORY https://github.com/nayuki/QR-Code-generator.git
    GIT_TAG v1.8.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(qrcodegen_source)
add_library(qrcodegen STATIC
    "${qrcodegen_source_SOURCE_DIR}/cpp/qrcodegen.cpp"
)
target_include_directories(qrcodegen PUBLIC
    "${qrcodegen_source_SOURCE_DIR}/cpp"
)

# 每个目录生成独立 target，避免使用全局 include/link 配置。依赖方向由各模块的
# target_link_libraries 明确表达，效果类似 Maven 多模块工程。
add_subdirectory(sdk)
add_subdirectory(core)
add_subdirectory(providers/demo)
add_subdirectory(apps/desktop-qt)
if(QUIZPANE_BUILD_BANK_STUDIO)
    add_subdirectory(apps/bank-studio)
endif()

if(QUIZPANE_BUILD_BANK_GENERATOR)
    add_subdirectory(tools/bank-generator)
endif()

if(QUIZPANE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

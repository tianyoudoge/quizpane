cmake_minimum_required(VERSION 3.24)

project(QuizPane VERSION 0.2.4 LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)

option(QUIZPANE_BUILD_TESTS "Build QuizPane tests" ON)
option(QUIZPANE_BUILD_BANK_GENERATOR
       "Build the experimental local question-bank generator" OFF)
option(QUIZPANE_BUILD_BANK_STUDIO "Build the QuizPane Question Maker GUI" ON)
option(QUIZPANE_ENABLE_DIAGNOSTIC_LOGGING
       "Write rotating diagnostic logs for local debug packages" OFF)
option(QUIZPANE_ENABLE_VERBOSE_DIAGNOSTICS
       "Include bounded source and model payloads in diagnostic logs" OFF)
if(QUIZPANE_ENABLE_VERBOSE_DIAGNOSTICS AND NOT QUIZPANE_ENABLE_DIAGNOSTIC_LOGGING)
    message(FATAL_ERROR "Verbose diagnostics require QUIZPANE_ENABLE_DIAGNOSTIC_LOGGING=ON")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network)
if(QUIZPANE_BUILD_BANK_STUDIO)
    # Core5Compat 只给题库制作器的文档提取器提供 GB18030 编码回退
    # （QStringConverter 内置编码不含 GB18030），其余模块不需要它。
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core5Compat Pdf)
endif()

option(QUIZPANE_ENABLE_TESSERACT_OCR
       "Enable the native Tesseract OCR backend in Question Maker" ON)
if(QUIZPANE_BUILD_BANK_STUDIO AND QUIZPANE_ENABLE_TESSERACT_OCR)
    # vcpkg（Windows）提供原生 CMake package；Homebrew 和多数 Linux
    # 发行版仍主要提供 pkg-config。统一成一个 target 供子目录链接。
    find_package(Tesseract CONFIG QUIET)
    if(TARGET Tesseract::libtesseract)
        set(QUIZPANE_TESSERACT_TARGET Tesseract::libtesseract)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(Tesseract REQUIRED IMPORTED_TARGET tesseract)
        set(QUIZPANE_TESSERACT_TARGET PkgConfig::Tesseract)
    endif()
endif()

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

# 题库包本质上是 ZIP。使用独立、稳定的 miniz API，避免依赖 Qt 的
# QZipReader/QZipWriter 私有接口；后者会随 Qt 补丁版本变化，官方二进制 SDK
# 也不保证提供，无法可靠进行跨平台 CI 构建。
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
# miniz 3.0.2 的源码仍声明 cmake_minimum_required(VERSION 3.0)。CMake 4
# 需要显式说明按 3.5 以后的策略解释它，避免影响我们自己的策略设置。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(miniz_source
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniz_source)

# 每个目录生成独立 target，避免使用全局 include/link 配置。依赖方向由各模块的
# target_link_libraries 明确表达，效果类似 Maven 多模块工程。core 必须先于 sdk
# 配置，因为 quizpane_provider_host 链接 quizpane_core 以复用 BankValidator，
# 避免声明式 Provider 和制作器/CLI 各写一份题库校验规则。
add_subdirectory(core)
add_subdirectory(sdk)
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

cmake_minimum_required(VERSION 3.24)

project(QuizPane VERSION 0.2.13 LANGUAGES C CXX)

if(WIN32)
    # 禁止 Windows SDK 注入 min/max 宏，避免破坏 std::min/std::max 和
    # std::numeric_limits<T>::max() 等现代 C++ API。对子目录所有目标生效。
    add_compile_definitions(NOMINMAX)
endif()

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
option(QUIZPANE_PORTABLE_CPU_BASELINE
       "Build release binaries for a conservative, portable CPU baseline" ON)
option(QUIZPANE_ENABLE_PCH "Use a shared precompiled header for C++ targets" ON)
option(QUIZPANE_ENABLE_UNITY_BUILD
       "Batch translation units for faster clean builds (best used in the fast preset)" OFF)
option(QUIZPANE_ENABLE_COMPILER_CACHE "Use ccache or sccache when available" ON)

if(QUIZPANE_ENABLE_COMPILER_CACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(QUIZPANE_COMPILER_CACHE NAMES sccache ccache)
    if(QUIZPANE_COMPILER_CACHE)
        set(CMAKE_C_COMPILER_LAUNCHER "${QUIZPANE_COMPILER_CACHE}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${QUIZPANE_COMPILER_CACHE}")
        message(STATUS "QuizPane compiler cache: ${QUIZPANE_COMPILER_CACHE}")
    endif()
endif()

function(quizpane_optimize_target target)
    if(QUIZPANE_ENABLE_PCH)
        target_precompile_headers(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:${PROJECT_SOURCE_DIR}/cmake/quizpane_pch.hxx>")
    endif()
    if(QUIZPANE_ENABLE_UNITY_BUILD)
        set_target_properties(${target} PROPERTIES
            UNITY_BUILD ON
            UNITY_BUILD_BATCH_SIZE 6)
    endif()
endfunction()
if(QUIZPANE_ENABLE_VERBOSE_DIAGNOSTICS AND NOT QUIZPANE_ENABLE_DIAGNOSTIC_LOGGING)
    message(FATAL_ERROR "Verbose diagnostics require QUIZPANE_ENABLE_DIAGNOSTIC_LOGGING=ON")
endif()

# 发行包不能继承构建机的 CPU 特性。比如 GitHub runner 上误用
# -march=native/-mavx2，到较旧的兆芯或海光机器上会直接 SIGILL。
# CMAKE_<LANG>_FLAGS 包含 CFLAGS/CXXFLAGS 环境变量的初始值，因此在这里
# 统一拒绝会破坏通用基线的显式参数。
if(QUIZPANE_PORTABLE_CPU_BASELINE)
    string(TOLOWER
        "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS_RELEASE} ${CMAKE_CXX_FLAGS_RELEASE}"
        QUIZPANE_CONFIGURED_CPU_FLAGS)
    if(QUIZPANE_CONFIGURED_CPU_FLAGS MATCHES
       "(^|[ ;])(-(march|mcpu)=[^ ;]+|-mtune=native|-m(avx|sse|ssse|fma|f16c|bmi|aes|sha|popcnt)[^ ;]*|/arch:avx[^ ;]*)")
        message(FATAL_ERROR
            "QUIZPANE_PORTABLE_CPU_BASELINE=ON forbids native/AVX/SSE-specific compiler flags: "
            "${QUIZPANE_CONFIGURED_CPU_FLAGS}")
    endif()

    # Windows x64 的 MSVC 默认基线已是 SSE2，不添加 /arch 才最兼容。
    # macOS 发行包按 arm64/x86_64 分开构建，交给 Apple toolchain 的
    # deployment target 管理基线。仅 Linux 需显式覆盖 GNU/Clang 的 CPU 选项。
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" QUIZPANE_SYSTEM_PROCESSOR)
        if(QUIZPANE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
            add_compile_options(-march=x86-64 -mtune=generic)
            message(STATUS "QuizPane portable CPU baseline: x86-64, generic tuning")
        elseif(QUIZPANE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            add_compile_options(-march=armv8-a)
            message(STATUS "QuizPane portable CPU baseline: ARMv8-A")
        else()
            message(STATUS
                "QuizPane portable CPU baseline: no explicit flags for "
                "${CMAKE_SYSTEM_PROCESSOR}; toolchain defaults apply")
        endif()
    endif()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network Concurrent)
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
    # v1.8.0 peeled commit；锁定不可变提交，避免 tag 被移动导致产物漂移。
    GIT_TAG 720f62bddb7226106071d4728c292cb1df519ceb
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
    GIT_TAG 293d4db1b7d0ffee9756d035b9ac6f7431ef8492
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

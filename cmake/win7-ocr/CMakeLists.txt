cmake_minimum_required(VERSION 3.24)
project(QuizPaneWin7OcrDependencies LANGUAGES C CXX)

# Isolated dependency build: never reuse Win10/v143/vcpkg binaries for Win7.
# Non-Windows builds are useful for checking the source patches and static API,
# but are NOT evidence of Windows 7 runtime compatibility.
if(WIN32)
    if(NOT MSVC OR NOT MSVC_TOOLSET_VERSION EQUAL 142)
        message(FATAL_ERROR "Win7 OCR requires MSVC 2019/v142")
    endif()
    set(api_flags " /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000")
endif()
include(ExternalProject)
set(OCR_INSTALL_DIR "${CMAKE_BINARY_DIR}/install" CACHE PATH "OCR install prefix")
set(common_args
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_INSTALL_PREFIX=${OCR_INSTALL_DIR}"
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_C_FLAGS=${api_flags}"
    "-DCMAKE_CXX_FLAGS=${api_flags}"
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"
    "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DSW_BUILD=OFF")

# Qt already decodes PDF pages to RGB. OCR needs no external image codecs,
# archive reader, network stack, training tools or OpenMP runtime.
ExternalProject_Add(leptonica
    URL https://codeload.github.com/DanBloomberg/leptonica/tar.gz/refs/tags/1.85.0
    URL_HASH SHA256=c01376bce0379d4ea4bc2ec5d5cbddaa49e2f06f88242619ab8c059e21adf233
    TLS_VERIFY TRUE
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CMAKE_ARGS ${common_args}
        -DBUILD_PROG=OFF -DENABLE_ZLIB=OFF -DENABLE_PNG=OFF
        -DENABLE_GIF=OFF -DENABLE_JPEG=OFF -DENABLE_TIFF=OFF
        -DENABLE_WEBP=OFF -DENABLE_OPENJPEG=OFF)
ExternalProject_Add(tesseract
    DEPENDS leptonica
    URL https://codeload.github.com/tesseract-ocr/tesseract/tar.gz/refs/tags/5.5.2
    URL_HASH SHA256=6235ea0dae45ea137f59c09320406f5888383741924d98855bd2ce0d16b54f21
    TLS_VERIFY TRUE
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/portable-tesseract.cmake"
    CMAKE_ARGS ${common_args}
        "-DCMAKE_PREFIX_PATH=${OCR_INSTALL_DIR}"
        "-DCMAKE_CXX_FLAGS=${api_flags} -DTESSERACT_DISABLE_DEBUG_FONTS"
        -DBUILD_TRAINING_TOOLS=OFF -DBUILD_TESTS=OFF
        -DOPENMP_BUILD=OFF -DENABLE_NATIVE=OFF -DENABLE_LTO=OFF
        -DGRAPHICS_DISABLED=ON -DDISABLE_ARCHIVE=ON -DDISABLE_CURL=ON
        -DDISABLE_TIFF=ON -DINSTALL_CONFIGS=OFF -DWIN32_MT_BUILD=OFF)

ExternalProject_Get_Property(leptonica SOURCE_DIR)
set(leptonica_source "${SOURCE_DIR}")
ExternalProject_Get_Property(tesseract SOURCE_DIR)
add_custom_target(ocr_licenses ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OCR_INSTALL_DIR}/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy "${leptonica_source}/leptonica-license.txt"
        "${OCR_INSTALL_DIR}/licenses/leptonica.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy "${SOURCE_DIR}/LICENSE"
        "${OCR_INSTALL_DIR}/licenses/tesseract.txt"
    DEPENDS tesseract)

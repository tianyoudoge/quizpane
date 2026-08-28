if(NOT PATCH_SCRIPT OR NOT TEST_BINARY_DIR)
    message(FATAL_ERROR "PATCH_SCRIPT and TEST_BINARY_DIR are required")
endif()

set(source_dir "${TEST_BINARY_DIR}/source")
file(REMOVE_RECURSE "${TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${source_dir}")
file(WRITE "${source_dir}/CMakeLists.txt" [=[
# Representative surrounding content from pinned Tesseract 5.5.2.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|x86_64|AMD64|amd64|i386|i686")
  message(FATAL_ERROR "SIMD block must be disabled by the Win7 patch")
endif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|x86_64|AMD64|amd64|i386|i686")

target_include_directories(
  libtesseract BEFORE
  PRIVATE src
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/arch>)
]=])

foreach(run RANGE 1 2)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${source_dir}" -P "${PATCH_SCRIPT}"
        RESULT_VARIABLE patch_result)
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Win7 Tesseract patch failed on run ${run}")
    endif()
endforeach()

file(READ "${source_dir}/CMakeLists.txt" patched)
if(NOT patched MATCHES "if\\(FALSE\\) # QuizPane Win7: portable scalar OCR")
    message(FATAL_ERROR "Win7 Tesseract patch did not disable the x86 SIMD block")
endif()
set(expected_install_include [=[$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>]=])
string(FIND "${patched}" "${expected_install_include}" install_include)
if(install_include EQUAL -1)
    message(FATAL_ERROR
        "Win7 Tesseract patch did not export the installed headers; consumers cannot include tesseract/baseapi.h")
endif()

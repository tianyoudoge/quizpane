# Pinned upstream CMake unconditionally enables x86 SIMD on MSVC, including
# /arch:AVX512 and experimental OpenMP flags. Use the existing generic branch
# instead. Fail closed on an upstream change; never silently skip this patch.
set(path "${SOURCE_DIR}/CMakeLists.txt")
file(READ "${path}" contents)
set(original "if(CMAKE_SYSTEM_PROCESSOR MATCHES \"x86|x86_64|AMD64|amd64|i386|i686\")")
set(patched "if(FALSE) # QuizPane Win7: portable scalar OCR, no x86 SIMD")
string(FIND "${contents}" "${patched}" already_patched)
if(already_patched EQUAL -1)
    string(FIND "${contents}" "\n${original}\n" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Pinned Tesseract SIMD block changed; review the Win7 CPU baseline")
    endif()
    string(REPLACE "\n${original}\n" "\n${patched}\n" contents "${contents}")
    # Match the named closing condition as well, for CMake's nesting check.
    string(REPLACE "endif(CMAKE_SYSTEM_PROCESSOR MATCHES \"x86|x86_64|AMD64|amd64|i386|i686\")"
        "endif()" contents "${contents}")
    file(WRITE "${path}" "${contents}")
endif()

# Win7 has no reduced-function distribution: keep the maker, PDF and OCR
# together even when callers bypass the packaging scripts or reuse a cache.
if(QUIZPANE_WINDOWS7_COMPAT)
    foreach(feature IN ITEMS QUIZPANE_BUILD_BANK_STUDIO
            QUIZPANE_ENABLE_QT_PDF QUIZPANE_ENABLE_TESSERACT_OCR)
        if(NOT ${feature})
            message(FATAL_ERROR "Win7 builds require ${feature}=ON (offline OCR is mandatory)")
        endif()
    endforeach()
endif()

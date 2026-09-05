foreach(disabled IN ITEMS NONE QUIZPANE_BUILD_BANK_STUDIO
        QUIZPANE_ENABLE_QT_PDF QUIZPANE_ENABLE_TESSERACT_OCR)
    set(arguments -DQUIZPANE_WINDOWS7_COMPAT=ON
        -DQUIZPANE_BUILD_BANK_STUDIO=ON -DQUIZPANE_ENABLE_QT_PDF=ON
        -DQUIZPANE_ENABLE_TESSERACT_OCR=ON)
    if(NOT disabled STREQUAL "NONE")
        list(APPEND arguments "-D${disabled}=OFF")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" ${arguments} -P "${POLICY_SCRIPT}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(disabled STREQUAL "NONE")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Full Win7 build rejected: ${error}")
        endif()
    elseif(result EQUAL 0 OR NOT error MATCHES "${disabled}=ON")
        message(FATAL_ERROR "Disabled Win7 feature was not rejected: ${disabled}: ${error}")
    endif()
endforeach()
# The restriction is specific to Win7; development on other platforms can
# still exercise extraction-disabled configurations.
execute_process(COMMAND "${CMAKE_COMMAND}" -DQUIZPANE_WINDOWS7_COMPAT=OFF
    -DQUIZPANE_BUILD_BANK_STUDIO=OFF -DQUIZPANE_ENABLE_QT_PDF=OFF
    -DQUIZPANE_ENABLE_TESSERACT_OCR=OFF -P "${POLICY_SCRIPT}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Win7 policy unexpectedly changed other platforms")
endif()

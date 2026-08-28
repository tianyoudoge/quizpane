if(NOT OCR_INSTALL_DIR)
    message(FATAL_ERROR "OCR_INSTALL_DIR is required")
endif()
set(base "https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/87416418657359cb625c412a48b6e1d6d41c29bd")
file(MAKE_DIRECTORY "${OCR_INSTALL_DIR}/tessdata" "${OCR_INSTALL_DIR}/licenses")
foreach(language IN ITEMS chi_sim eng)
    if(language STREQUAL "chi_sim")
        set(hash a5fcb6f0db1e1d6d8522f39db4e848f05984669172e584e8d76b6b3141e1f730)
    else()
        set(hash 7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2)
    endif()
    file(DOWNLOAD "${base}/${language}.traineddata"
        "${OCR_INSTALL_DIR}/tessdata/${language}.traineddata"
        EXPECTED_HASH "SHA256=${hash}" TLS_VERIFY ON TIMEOUT 120)
endforeach()
# tessdata_fast is Apache-2.0, as is Tesseract. Ship the license and provenance.
file(COPY_FILE "${OCR_INSTALL_DIR}/licenses/tesseract.txt"
    "${OCR_INSTALL_DIR}/licenses/tessdata-fast.txt")
file(WRITE "${OCR_INSTALL_DIR}/licenses/win7-ocr-versions.txt"
    "Experimental Win7 OCR; runtime validation on Windows 7 SP1 is required.\n"
    "Tesseract 5.5.2 (Apache-2.0), portable x86 scalar build\n"
    "Leptonica 1.85.0 (BSD-2-Clause), raw RGB only\n"
    "tessdata_fast 87416418657359cb625c412a48b6e1d6d41c29bd (Apache-2.0)\n"
    "chi_sim SHA256 a5fcb6f0db1e1d6d8522f39db4e848f05984669172e584e8d76b6b3141e1f730\n"
    "eng SHA256 7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2\n")

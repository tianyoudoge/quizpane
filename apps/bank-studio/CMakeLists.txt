add_library(question_maker_engine STATIC
    engine/include/quizpane/studio/document_extractor.hpp
    engine/include/quizpane/studio/generation_workflow.hpp
    engine/include/quizpane/studio/mineru_client.hpp
    engine/include/quizpane/studio/mineru_output_adapter.hpp
    engine/include/quizpane/studio/option_label.hpp
    engine/include/quizpane/studio/qt_pdf_compat.hpp
    engine/include/quizpane/studio/rule_based_generator.hpp
    engine/include/quizpane/studio/review_result.hpp
    src/review_draft_bank.hpp
    engine/src/document_extractor.cpp
    engine/src/generation_workflow.cpp
    engine/src/mineru_client.cpp
    engine/src/mineru_output_adapter.cpp
    engine/src/option_label.cpp
    engine/src/rule_based_generator.cpp
    src/review_draft_bank.cpp
)
target_include_directories(question_maker_engine PUBLIC
    engine/include
    src
)
target_link_libraries(question_maker_engine PUBLIC
    quizpane_core miniz
    ${QUIZPANE_QT_PACKAGE}::Core
    ${QUIZPANE_QT_PACKAGE}::Gui
    ${QUIZPANE_QT_PACKAGE}::Network
    ${QUIZPANE_QT_PACKAGE}::Concurrent
)
if(QUIZPANE_ENABLE_QT_PDF)
    if(QUIZPANE_QT_MAJOR_VERSION STREQUAL "5")
        target_link_libraries(question_maker_engine PUBLIC quizpane_qt5_pdf)
    else()
        target_link_libraries(question_maker_engine PUBLIC ${QUIZPANE_QT_PACKAGE}::Pdf)
    endif()
    target_compile_definitions(question_maker_engine PUBLIC QUIZPANE_HAS_QT_PDF=1)
endif()
if(QUIZPANE_QT_MAJOR_VERSION STREQUAL "6")
    target_link_libraries(question_maker_engine PUBLIC Qt6::Core5Compat)
endif()
quizpane_optimize_target(question_maker_engine)
if(QUIZPANE_ENABLE_TESSERACT_OCR)
    target_link_libraries(question_maker_engine PRIVATE ${QUIZPANE_TESSERACT_TARGET})
    target_compile_definitions(question_maker_engine PRIVATE QUIZPANE_HAS_TESSERACT_OCR=1)
endif()

quizpane_add_executable(quizpane_bank_studio
    src/main.cpp
    src/mineru_settings_dialog.cpp
    src/mineru_settings_dialog.hpp
    src/review_draft_bank.hpp
    src/source_row_widget.cpp
    src/source_row_widget.hpp
    src/source_validation.hpp
    src/styled_dropdown.cpp
    src/styled_dropdown.hpp
    src/studio_window.cpp
    src/studio_window.hpp
    resources/resources.qrc
)
# 两个 UI 文件各有匿名命名空间 helper；单独编译云解析设置，避免 Unity
# 把同名内部函数合并到同一翻译单元。
set_source_files_properties(src/mineru_settings_dialog.cpp PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON)
if(WIN32)
    target_sources(quizpane_bank_studio PRIVATE resources/windows.rc)
endif()
target_link_libraries(quizpane_bank_studio PRIVATE
    question_maker_engine quizpane_provider_host
    ${QUIZPANE_QT_PACKAGE}::Core ${QUIZPANE_QT_PACKAGE}::Network ${QUIZPANE_QT_PACKAGE}::Widgets
)
quizpane_optimize_target(quizpane_bank_studio)
quizpane_apply_windows7_manifest(quizpane_bank_studio)
target_compile_definitions(quizpane_bank_studio PRIVATE
    QUIZPANE_VERSION="${PROJECT_VERSION}"
)
set_target_properties(quizpane_bank_studio PROPERTIES
    AUTORCC TRUE
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "题库制作器"
    MACOSX_BUNDLE_BUNDLE_NAME "题库制作器"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.quizpane.bank-studio"
    MACOSX_BUNDLE_ICON_FILE "app-icon.icns"
)
if(APPLE)
    target_sources(quizpane_bank_studio PRIVATE
        resources/app-icon.icns
        "${PROJECT_SOURCE_DIR}/LICENSE"
    )
    set_source_files_properties(
        resources/app-icon.icns
        "${PROJECT_SOURCE_DIR}/LICENSE"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
endif()

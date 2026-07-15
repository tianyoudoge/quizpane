add_library(question_maker_engine STATIC
    engine/include/quizpane/studio/checkpoint_store.hpp
    engine/include/quizpane/studio/chunker.hpp
    engine/include/quizpane/studio/document_extractor.hpp
    engine/include/quizpane/studio/generation_workflow.hpp
    engine/include/quizpane/studio/model_client.hpp
    engine/include/quizpane/studio/option_label.hpp
    engine/include/quizpane/studio/rule_based_generator.hpp
    engine/include/quizpane/studio/review_result.hpp
    engine/src/checkpoint_store.cpp
    engine/src/chunker.cpp
    engine/src/document_extractor.cpp
    engine/src/generation_workflow.cpp
    engine/src/model_client.cpp
    engine/src/option_label.cpp
    engine/src/rule_based_generator.cpp
)
target_include_directories(question_maker_engine PUBLIC
    engine/include
    src
)
target_link_libraries(question_maker_engine PUBLIC
    quizpane_core miniz Qt6::Core Qt6::Core5Compat Qt6::Gui Qt6::Network Qt6::Pdf Qt6::Concurrent
)
quizpane_optimize_target(question_maker_engine)
if(QUIZPANE_ENABLE_TESSERACT_OCR)
    target_link_libraries(question_maker_engine PRIVATE ${QUIZPANE_TESSERACT_TARGET})
    target_compile_definitions(question_maker_engine PRIVATE QUIZPANE_HAS_TESSERACT_OCR=1)
endif()

qt_add_executable(quizpane_bank_studio
    src/main.cpp
    src/model_settings_dialog.cpp
    src/model_settings_dialog.hpp
    src/source_row_widget.cpp
    src/source_row_widget.hpp
    src/source_validation.hpp
    src/styled_dropdown.cpp
    src/styled_dropdown.hpp
    src/studio_window.cpp
    src/studio_window.hpp
    resources/resources.qrc
)
# 两个 UI 文件各有匿名命名空间 helper；单独编译 model settings，避免 Unity
# 把同名内部函数合并到同一翻译单元。
set_source_files_properties(src/model_settings_dialog.cpp PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON)
if(WIN32)
    target_sources(quizpane_bank_studio PRIVATE resources/windows.rc)
endif()
target_link_libraries(quizpane_bank_studio PRIVATE
    question_maker_engine quizpane_provider_host
    Qt6::Core Qt6::Network Qt6::Widgets
)
quizpane_optimize_target(quizpane_bank_studio)
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

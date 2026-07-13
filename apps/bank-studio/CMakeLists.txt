qt_add_executable(quizpane_bank_studio
    src/main.cpp
    src/model_settings_dialog.cpp
    src/model_settings_dialog.hpp
    src/studio_window.cpp
    src/studio_window.hpp
    resources/resources.qrc
)
if(WIN32)
    target_sources(quizpane_bank_studio PRIVATE resources/windows.rc)
endif()
target_link_libraries(quizpane_bank_studio PRIVATE Qt6::Core Qt6::Network Qt6::Widgets)
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

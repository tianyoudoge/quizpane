qt_add_executable(quizpane_bank_studio
    src/main.cpp
    src/studio_window.cpp
    src/studio_window.hpp
    resources/resources.qrc
)
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
        ../desktop-qt/resources/app-icon.icns
        "${PROJECT_SOURCE_DIR}/LICENSE"
    )
    set_source_files_properties(
        ../desktop-qt/resources/app-icon.icns
        "${PROJECT_SOURCE_DIR}/LICENSE"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
endif()

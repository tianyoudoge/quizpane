qt_add_executable(quizpane
    src/main.cpp
    src/platform/file_association.cpp
    src/platform/file_association.hpp
    src/platform/global_hotkey.cpp
    src/platform/global_hotkey.hpp
    src/platform/window_pinning.hpp
    src/ui/main_window.cpp
    src/ui/main_window.hpp
    src/ui/line_icons.cpp
    src/ui/line_icons.hpp
    src/ui/app_dialogs.cpp
    src/ui/app_dialogs.hpp
    resources/resources.qrc
)
if(APPLE)
    enable_language(OBJCXX)
    target_sources(quizpane PRIVATE
        src/platform/window_pinning.mm
        resources/app-icon.icns
        "${PROJECT_SOURCE_DIR}/LICENSE"
    )
    set_source_files_properties(
        resources/app-icon.icns
        "${PROJECT_SOURCE_DIR}/LICENSE"
        PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_link_libraries(quizpane PRIVATE "-framework Carbon" "-framework AppKit")
elseif(WIN32)
    target_sources(quizpane PRIVATE
        src/platform/window_pinning.cpp
        resources/windows.rc
    )
    target_include_directories(quizpane PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    )
    target_link_libraries(quizpane PRIVATE User32 Shell32)
elseif(UNIX)
    target_sources(quizpane PRIVATE src/platform/window_pinning.cpp)
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_compile_definitions(quizpane PRIVATE QUIZPANE_HAVE_X11=1)
        target_link_libraries(quizpane PRIVATE X11::X11)
    endif()
endif()
target_link_libraries(quizpane PRIVATE
    quizpane_core
    quizpane_provider_host
    qrcodegen
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
)
target_compile_definitions(quizpane PRIVATE
    QUIZPANE_VERSION="${PROJECT_VERSION}"
)
set_target_properties(quizpane PROPERTIES
    AUTORCC TRUE
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "小窗刷题"
    MACOSX_BUNDLE_BUNDLE_NAME "小窗刷题"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.quizpane.app"
    MACOSX_BUNDLE_ICON_FILE "app-icon.icns"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
)

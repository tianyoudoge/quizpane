add_library(quizpane_provider_demo SHARED demo_provider.cpp)
target_compile_definitions(quizpane_provider_demo PRIVATE QUIZPANE_PROVIDER_BUILD)
target_link_libraries(quizpane_provider_demo PRIVATE quizpane_provider_sdk ${QUIZPANE_QT_PACKAGE}::Core)
set_target_properties(quizpane_provider_demo PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

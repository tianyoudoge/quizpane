add_library(quizpane_core STATIC
    src/draft_store.cpp
    src/image_privacy_filter.cpp
    src/attempt.cpp
)
target_include_directories(quizpane_core PUBLIC include)
target_link_libraries(quizpane_core PUBLIC Qt6::Core Qt6::Gui)

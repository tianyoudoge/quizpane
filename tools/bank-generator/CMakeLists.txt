add_executable(quizpane-bank-generator main.cpp)
target_link_libraries(quizpane-bank-generator PRIVATE quizpane_provider_host quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)

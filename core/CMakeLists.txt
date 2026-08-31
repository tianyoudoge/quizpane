add_library(quizpane_core STATIC
    src/diagnostic_logger.cpp
    src/draft_store.cpp
    src/feedback_report.cpp
    src/image_privacy_filter.cpp
    src/io_utils.cpp
    src/provider_response_router.cpp
    src/attempt.cpp
    src/bank_validator.cpp
)
# 诊断实现启用时会包含平台崩溃处理头，保持独立编译以隔离系统宏。
set_source_files_properties(src/diagnostic_logger.cpp PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(quizpane_core PUBLIC include)
target_link_libraries(quizpane_core PUBLIC ${QUIZPANE_QT_PACKAGE}::Core ${QUIZPANE_QT_PACKAGE}::Gui ${QUIZPANE_QT_PACKAGE}::Network)
quizpane_optimize_target(quizpane_core)
if(QUIZPANE_ENABLE_DIAGNOSTIC_LOGGING)
    target_compile_definitions(quizpane_core PUBLIC QUIZPANE_DIAGNOSTIC_LOGGING=1)
    if(WIN32)
        target_link_libraries(quizpane_core PUBLIC Dbghelp Psapi)
    endif()
endif()
if(QUIZPANE_ENABLE_PROD_DIAGNOSTICS)
    target_compile_definitions(quizpane_core PUBLIC QUIZPANE_PROD_DIAGNOSTICS=1)
    if(WIN32)
        target_link_libraries(quizpane_core PUBLIC Dbghelp Psapi)
    endif()
endif()
if(QUIZPANE_ENABLE_VERBOSE_DIAGNOSTICS)
    target_compile_definitions(quizpane_core PUBLIC QUIZPANE_VERBOSE_DIAGNOSTICS=1)
endif()

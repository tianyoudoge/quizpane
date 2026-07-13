add_executable(provider_loader_test provider_loader_test.cpp)
target_link_libraries(provider_loader_test PRIVATE
    quizpane_provider_host Qt6::Core
)
target_compile_definitions(provider_loader_test PRIVATE
    DEMO_PROVIDER_PATH="$<TARGET_FILE:quizpane_provider_demo>"
)
add_dependencies(provider_loader_test quizpane_provider_demo)
add_test(NAME provider_loader_test COMMAND provider_loader_test)

add_executable(attempt_roundtrip_test attempt_roundtrip_test.cpp)
target_link_libraries(attempt_roundtrip_test PRIVATE quizpane_core Qt6::Core)
add_test(NAME attempt_roundtrip_test COMMAND attempt_roundtrip_test)

add_executable(draft_store_test draft_store_test.cpp)
target_link_libraries(draft_store_test PRIVATE quizpane_core Qt6::Core)
add_test(NAME draft_store_test COMMAND draft_store_test)

add_executable(image_privacy_filter_test image_privacy_filter_test.cpp)
target_link_libraries(image_privacy_filter_test PRIVATE
    quizpane_core Qt6::Core Qt6::Gui
)
add_test(NAME image_privacy_filter_test COMMAND image_privacy_filter_test)

add_executable(provider_installer_test provider_installer_test.cpp)
target_link_libraries(provider_installer_test PRIVATE
    quizpane_provider_host Qt6::Core Qt6::CorePrivate
)
add_test(NAME provider_installer_test COMMAND provider_installer_test)

add_executable(provider_rpc_smoke provider_rpc_smoke.cpp)
target_link_libraries(provider_rpc_smoke PRIVATE quizpane_provider_host Qt6::Core)

add_executable(provider_flow_test provider_flow_test.cpp)
target_link_libraries(provider_flow_test PRIVATE quizpane_provider_host Qt6::Core)
target_compile_definitions(provider_flow_test PRIVATE
    DEMO_PROVIDER_PATH="$<TARGET_FILE:quizpane_provider_demo>"
)
add_dependencies(provider_flow_test quizpane_provider_demo)
add_test(NAME provider_flow_test COMMAND provider_flow_test)

add_executable(declarative_provider_flow_test declarative_provider_flow_test.cpp)
target_link_libraries(declarative_provider_flow_test PRIVATE
    quizpane_provider_host Qt6::Core
)
target_compile_definitions(declarative_provider_flow_test PRIVATE
    DECLARATIVE_BANK_PATH="${PROJECT_SOURCE_DIR}/schemas/examples/declarative/content/bank.json"
)
add_test(NAME declarative_provider_flow_test COMMAND declarative_provider_flow_test)

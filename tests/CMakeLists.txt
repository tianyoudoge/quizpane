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
    quizpane_provider_host Qt6::Core
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

add_executable(declarative_provider_materials_test declarative_provider_materials_test.cpp)
target_link_libraries(declarative_provider_materials_test PRIVATE
    quizpane_provider_host Qt6::Core
)
target_compile_definitions(declarative_provider_materials_test PRIVATE
    DECLARATIVE_MATERIALS_BANK_PATH="${PROJECT_SOURCE_DIR}/schemas/examples/declarative-materials/content/bank.json"
)
add_test(NAME declarative_provider_materials_test COMMAND declarative_provider_materials_test)

add_executable(bank_validator_test bank_validator_test.cpp)
target_link_libraries(bank_validator_test PRIVATE quizpane_core Qt6::Core)
target_compile_definitions(bank_validator_test PRIVATE
    DECLARATIVE_BANK_PATH="${PROJECT_SOURCE_DIR}/schemas/examples/declarative/content/bank.json"
    DECLARATIVE_MATERIALS_BANK_PATH="${PROJECT_SOURCE_DIR}/schemas/examples/declarative-materials/content/bank.json"
)
add_test(NAME bank_validator_test COMMAND bank_validator_test)

add_executable(material_card_test
    material_card_test.cpp
    ../apps/desktop-qt/src/ui/material_card.cpp
    ../apps/desktop-qt/src/ui/line_icons.cpp
)
target_link_libraries(material_card_test PRIVATE Qt6::Widgets)
add_test(NAME material_card_test COMMAND material_card_test)

if(TARGET question_maker_engine)
    add_executable(document_extractor_test document_extractor_test.cpp)
    target_link_libraries(document_extractor_test PRIVATE question_maker_engine Qt6::Core)
    add_test(NAME document_extractor_test COMMAND document_extractor_test)

    add_executable(checkpoint_store_test checkpoint_store_test.cpp)
    target_link_libraries(checkpoint_store_test PRIVATE question_maker_engine Qt6::Core)
    add_test(NAME checkpoint_store_test COMMAND checkpoint_store_test)

    add_executable(model_client_test model_client_test.cpp)
    target_link_libraries(model_client_test PRIVATE question_maker_engine Qt6::Core Qt6::Network)
    add_test(NAME model_client_test COMMAND model_client_test)

    add_executable(chunker_test chunker_test.cpp)
    target_link_libraries(chunker_test PRIVATE question_maker_engine Qt6::Core)
    add_test(NAME chunker_test COMMAND chunker_test)
endif()

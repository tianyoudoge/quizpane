add_executable(provider_loader_test provider_loader_test.cpp)
target_link_libraries(provider_loader_test PRIVATE
    quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core
)
target_compile_definitions(provider_loader_test PRIVATE
    DEMO_PROVIDER_PATH="$<TARGET_FILE:quizpane_provider_demo>"
)
add_dependencies(provider_loader_test quizpane_provider_demo)
add_test(NAME provider_loader_test COMMAND provider_loader_test)

add_executable(attempt_roundtrip_test attempt_roundtrip_test.cpp)
target_link_libraries(attempt_roundtrip_test PRIVATE quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)
add_test(NAME attempt_roundtrip_test COMMAND attempt_roundtrip_test)

add_executable(draft_store_test draft_store_test.cpp)
target_link_libraries(draft_store_test PRIVATE quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)
add_test(NAME draft_store_test COMMAND draft_store_test)

add_executable(provider_response_router_test provider_response_router_test.cpp)
target_link_libraries(provider_response_router_test PRIVATE quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)
add_test(NAME provider_response_router_test COMMAND provider_response_router_test)

add_executable(image_privacy_filter_test image_privacy_filter_test.cpp)
target_link_libraries(image_privacy_filter_test PRIVATE
    quizpane_core ${QUIZPANE_QT_PACKAGE}::Core ${QUIZPANE_QT_PACKAGE}::Gui
)
add_test(NAME image_privacy_filter_test COMMAND image_privacy_filter_test)

add_executable(provider_installer_test provider_installer_test.cpp)
target_link_libraries(provider_installer_test PRIVATE
    quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core
)
add_test(NAME provider_installer_test COMMAND provider_installer_test)

add_executable(running_app_handoff_test running_app_handoff_test.cpp)
target_link_libraries(running_app_handoff_test PRIVATE
    quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core ${QUIZPANE_QT_PACKAGE}::Network
)
add_test(NAME running_app_handoff_test COMMAND running_app_handoff_test)

add_executable(provider_rpc_smoke provider_rpc_smoke.cpp)
target_link_libraries(provider_rpc_smoke PRIVATE quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core)

add_executable(provider_flow_test provider_flow_test.cpp)
target_link_libraries(provider_flow_test PRIVATE quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core)
target_compile_definitions(provider_flow_test PRIVATE
    DEMO_PROVIDER_PATH="$<TARGET_FILE:quizpane_provider_demo>"
)
add_dependencies(provider_flow_test quizpane_provider_demo)
add_test(NAME provider_flow_test COMMAND provider_flow_test)

add_executable(declarative_provider_flow_test declarative_provider_flow_test.cpp)
target_link_libraries(declarative_provider_flow_test PRIVATE
    quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core
)
target_compile_definitions(declarative_provider_flow_test PRIVATE
    DECLARATIVE_BANK_PATH="${PROJECT_SOURCE_DIR}/schemas/examples/declarative/content/bank.json"
)
add_test(NAME declarative_provider_flow_test COMMAND declarative_provider_flow_test)

add_executable(declarative_provider_materials_test declarative_provider_materials_test.cpp)
target_link_libraries(declarative_provider_materials_test PRIVATE
    quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core
)
target_compile_definitions(declarative_provider_materials_test PRIVATE
    DECLARATIVE_MATERIALS_BANK_PATH="${PROJECT_SOURCE_DIR}/schemas/examples/declarative-materials/content/bank.json"
)
add_test(NAME declarative_provider_materials_test COMMAND declarative_provider_materials_test)

add_executable(material_package_e2e_test material_package_e2e_test.cpp)
target_link_libraries(material_package_e2e_test PRIVATE
    quizpane_provider_host quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)
target_compile_definitions(material_package_e2e_test PRIVATE
    DECLARATIVE_MATERIALS_BANK_PATH="${PROJECT_SOURCE_DIR}/schemas/examples/declarative-materials/content/bank.json"
)
add_test(NAME material_package_e2e_test COMMAND material_package_e2e_test)

add_executable(bank_validator_test bank_validator_test.cpp)
target_link_libraries(bank_validator_test PRIVATE quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)
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
target_link_libraries(material_card_test PRIVATE ${QUIZPANE_QT_PACKAGE}::Widgets)
add_test(NAME material_card_test COMMAND material_card_test)

add_executable(question_navigator_test
    question_navigator_test.cpp
    ../apps/desktop-qt/src/ui/question_navigator.cpp
)
target_link_libraries(question_navigator_test PRIVATE ${QUIZPANE_QT_PACKAGE}::Widgets)
add_test(NAME question_navigator_test COMMAND question_navigator_test)

add_executable(browser_protocol_test
    browser_protocol_test.cpp
    ../apps/desktop-qt/src/browser/browser_protocol.cpp
)
target_link_libraries(browser_protocol_test PRIVATE ${QUIZPANE_QT_PACKAGE}::Core)
add_test(NAME browser_protocol_test COMMAND browser_protocol_test)

if(TARGET question_maker_engine)
    add_executable(studio_review_ui_test
        studio_review_ui_test.cpp
        ../apps/bank-studio/src/studio_window.cpp
        ../apps/bank-studio/src/studio_window.hpp
        ../apps/bank-studio/src/model_settings_dialog.cpp
        ../apps/bank-studio/src/source_row_widget.cpp
        ../apps/bank-studio/src/styled_dropdown.cpp
        ../apps/bank-studio/resources/resources.qrc
    )
    set_target_properties(studio_review_ui_test PROPERTIES AUTORCC TRUE)
    target_link_libraries(studio_review_ui_test PRIVATE
        question_maker_engine quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Widgets)
    add_test(NAME studio_review_ui_test COMMAND studio_review_ui_test)

    add_executable(review_draft_bank_test review_draft_bank_test.cpp)
    target_link_libraries(review_draft_bank_test PRIVATE
        question_maker_engine quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)
    target_compile_definitions(review_draft_bank_test PRIVATE
        DECLARATIVE_BANK_PATH="${PROJECT_SOURCE_DIR}/schemas/examples/declarative/content/bank.json"
    )
    add_test(NAME review_draft_bank_test COMMAND review_draft_bank_test)

    add_executable(document_extractor_test document_extractor_test.cpp)
    target_link_libraries(document_extractor_test PRIVATE
        question_maker_engine quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core ${QUIZPANE_QT_PACKAGE}::Gui)
    target_compile_definitions(document_extractor_test PRIVATE
        DOCUMENT_EXTRACTOR_OCR_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ocr-scan-fixture.pdf"
    )
    if(QUIZPANE_ENABLE_TESSERACT_OCR)
        target_compile_definitions(document_extractor_test PRIVATE
            DOCUMENT_EXTRACTOR_HAS_OCR=1)
    endif()
    add_test(NAME document_extractor_test COMMAND document_extractor_test)

    add_executable(rule_based_generator_test rule_based_generator_test.cpp)
    target_link_libraries(rule_based_generator_test PRIVATE
        question_maker_engine quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core)
    add_test(NAME rule_based_generator_test COMMAND rule_based_generator_test)

    add_executable(embedded_answer_test embedded_answer_test.cpp)
    target_link_libraries(embedded_answer_test PRIVATE
        question_maker_engine quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)
    add_test(NAME embedded_answer_test COMMAND embedded_answer_test)

    # Manual regression harness for real paired PDFs. Callers provide the PDFs and an
    # isolated output directory, so it is intentionally not registered with CTest.
    add_executable(pdf_regression_harness pdf_regression_harness.cpp)
    target_link_libraries(pdf_regression_harness PRIVATE
        question_maker_engine quizpane_provider_host ${QUIZPANE_QT_PACKAGE}::Core)

    add_executable(checkpoint_store_test checkpoint_store_test.cpp)
    target_link_libraries(checkpoint_store_test PRIVATE question_maker_engine ${QUIZPANE_QT_PACKAGE}::Core)
    add_test(NAME checkpoint_store_test COMMAND checkpoint_store_test)

    add_executable(model_client_test model_client_test.cpp)
    target_link_libraries(model_client_test PRIVATE question_maker_engine ${QUIZPANE_QT_PACKAGE}::Core ${QUIZPANE_QT_PACKAGE}::Network)
    add_test(NAME model_client_test COMMAND model_client_test)

    add_executable(chunker_test chunker_test.cpp)
    target_link_libraries(chunker_test PRIVATE question_maker_engine ${QUIZPANE_QT_PACKAGE}::Core)
    add_test(NAME chunker_test COMMAND chunker_test)

    add_executable(generation_workflow_test generation_workflow_test.cpp)
    target_link_libraries(generation_workflow_test PRIVATE
        question_maker_engine ${QUIZPANE_QT_PACKAGE}::Core)
    add_test(NAME generation_workflow_test COMMAND generation_workflow_test)

if(QUIZPANE_ENABLE_DIAGNOSTIC_LOGGING OR QUIZPANE_ENABLE_PROD_DIAGNOSTICS)
    add_executable(diagnostic_logger_test diagnostic_logger_test.cpp)
    target_link_libraries(diagnostic_logger_test PRIVATE quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)
    add_test(NAME diagnostic_logger_test COMMAND diagnostic_logger_test)
    add_executable(diagnostic_crash_test diagnostic_crash_test.cpp)
    target_link_libraries(diagnostic_crash_test PRIVATE quizpane_core ${QUIZPANE_QT_PACKAGE}::Core)
    add_test(NAME diagnostic_crash_test COMMAND diagnostic_crash_test)
    add_executable(feedback_report_test feedback_report_test.cpp)
    target_link_libraries(feedback_report_test PRIVATE
        quizpane_core ${QUIZPANE_QT_PACKAGE}::Core ${QUIZPANE_QT_PACKAGE}::Network)
    add_test(NAME feedback_report_test COMMAND feedback_report_test)
endif()
endif()

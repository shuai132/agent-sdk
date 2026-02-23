cmake_minimum_required(VERSION 3.20)
project(agent-sdk VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Project name without hyphens for target names
set(AGENT_SDK_NAME agent_sdk)
set(AGENT_CLI_NAME agent_cli)

# Options (only enabled by default when this is the top-level project)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(AGENT_BUILD_TESTS "Build tests" ON)
    option(AGENT_BUILD_EXAMPLES "Build examples" ON)
    option(AGENT_BUILD_CLI "Build agent_cli TUI application" ON)
    option(AGENT_PLUGIN_QWEN "Build Qwen OAuth plugin" ON)
else ()
    option(AGENT_BUILD_TESTS "Build tests" OFF)
    option(AGENT_BUILD_EXAMPLES "Build examples" OFF)
    option(AGENT_BUILD_CLI "Build agent_cli TUI application" OFF)
    option(AGENT_PLUGIN_QWEN "Build Qwen OAuth plugin" OFF)
endif ()

# Third-party dependencies via git submodules
set(THIRDPARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)

# nlohmann/json
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/json)

# asio (standalone, header-only)
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${THIRDPARTY_DIR}/asio/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE)

# spdlog for logging
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/spdlog)

# FTXUI for TUI applications
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/ftxui)

# OpenSSL for HTTPS
find_package(OpenSSL REQUIRED)

# Collect plugin sources
set(PLUGIN_SOURCES
        src/plugin/auth_provider.cpp
)

if (AGENT_PLUGIN_QWEN)
    list(APPEND PLUGIN_SOURCES
            src/plugin/qwen/qwen_oauth.cpp
            src/plugin/qrcode.cpp
            thirdparty/QRCode/src/qrcode.c
    )
    message(STATUS "Qwen OAuth plugin: ENABLED")
else ()
    message(STATUS "Qwen OAuth plugin: DISABLED")
endif ()

# Main library
add_library(${AGENT_SDK_NAME}
        # Core types
        src/core/types.cpp
        src/core/message.cpp
        src/core/config.cpp
        src/core/json_store.cpp
        src/core/uuid.cpp

        # Log
        src/log/log.cpp

        # Event bus
        src/bus/bus.cpp

        # Network layer
        src/net/http_client.cpp
        src/net/sse_client.cpp

        # LLM providers
        src/llm/provider.cpp
        src/llm/anthropic.cpp
        src/llm/openai.cpp

        # Tool system
        src/tool/registry.cpp
        src/tool/tool.cpp
        src/tool/permission.cpp
        src/tool/builtin/bash.cpp
        src/tool/builtin/read.cpp
        src/tool/builtin/write.cpp
        src/tool/builtin/edit.cpp
        src/tool/builtin/glob.cpp
        src/tool/builtin/grep.cpp
        src/tool/builtin/task.cpp
        src/tool/builtin/question.cpp

        # Session management
        src/session/session.cpp
        src/session/prompt.cpp
        src/session/processor.cpp
        src/session/compaction.cpp
        src/session/truncate.cpp

        # Agent system
        src/agent/agent.cpp

        # MCP client
        src/mcp/client.cpp
        src/mcp/transport.cpp

        # Plugin system
        ${PLUGIN_SOURCES}

        # Skill system
        src/skill/skill.cpp
        src/tool/builtin/skill.cpp
)

# Add compile definition for Qwen plugin
if (AGENT_PLUGIN_QWEN)
    target_compile_definitions(${AGENT_SDK_NAME} PUBLIC AGENT_PLUGIN_QWEN=1)
endif ()

target_include_directories(${AGENT_SDK_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${THIRDPARTY_DIR}/QRCode/src
)

target_link_libraries(${AGENT_SDK_NAME} PUBLIC
        nlohmann_json::nlohmann_json
        asio
        spdlog::spdlog
        OpenSSL::SSL
        OpenSSL::Crypto
)

# Platform-specific settings
if (APPLE)
    target_link_libraries(${AGENT_SDK_NAME} PUBLIC "-framework CoreFoundation" "-framework Security")
endif ()

if (UNIX AND NOT APPLE)
    target_link_libraries(${AGENT_SDK_NAME} PUBLIC pthread)
endif ()

# Examples
if (AGENT_BUILD_EXAMPLES)
    add_executable(${AGENT_SDK_NAME}_simple_chat examples/simple_chat.cpp)
    target_link_libraries(${AGENT_SDK_NAME}_simple_chat PRIVATE ${AGENT_SDK_NAME})

    add_executable(${AGENT_SDK_NAME}_api_test examples/api_test.cpp)
    target_link_libraries(${AGENT_SDK_NAME}_api_test PRIVATE ${AGENT_SDK_NAME})

    add_executable(${AGENT_SDK_NAME}_tool_test examples/tool_test.cpp)
    target_link_libraries(${AGENT_SDK_NAME}_tool_test PRIVATE ${AGENT_SDK_NAME})

    if (AGENT_PLUGIN_QWEN)
        add_executable(${AGENT_SDK_NAME}_qwen_oauth_test examples/qwen_oauth_test.cpp)
        target_link_libraries(${AGENT_SDK_NAME}_qwen_oauth_test PRIVATE ${AGENT_SDK_NAME})
    endif ()
endif ()

# CLI TUI application
if (AGENT_BUILD_CLI)
    add_executable(${AGENT_CLI_NAME}
            tui/agent_cli.cpp
            tui/tui_components.cpp
            tui/tui_state.cpp
            tui/tui_callbacks.cpp
            tui/tui_render.cpp
            tui/tui_event_handler.cpp
    )
    target_include_directories(${AGENT_CLI_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tui)
    target_link_libraries(${AGENT_CLI_NAME} PRIVATE ${AGENT_SDK_NAME} ftxui::component)
endif ()

# Tests
if (AGENT_BUILD_TESTS)
    enable_testing()

    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${THIRDPARTY_DIR}/googletest)

    add_executable(${AGENT_SDK_NAME}_tests
            tests/test_main.cpp
            tests/test_message.cpp
            tests/test_tool.cpp
            tests/test_session.cpp
            tests/test_llm.cpp
            tests/test_json_store.cpp
            tests/test_skill.cpp
            tests/test_bus.cpp
            tests/test_types.cpp
            tests/test_config.cpp
            tests/test_mcp.cpp
            tests/test_permission.cpp
            tests/test_builtin_tools.cpp
            tests/test_net.cpp
            tests/test_agent_cli.cpp
            tests/test_history_logic.cpp
            tests/test_plugin_auth.cpp
            # TUI components for CLI tests
            tui/tui_components.cpp
    )

    target_include_directories(${AGENT_SDK_NAME}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tui)
    target_link_libraries(${AGENT_SDK_NAME}_tests PRIVATE
            ${AGENT_SDK_NAME}
            GTest::gtest
    )

    include(GoogleTest)
    gtest_discover_tests(${AGENT_SDK_NAME}_tests)
endif ()

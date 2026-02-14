cmake_minimum_required(VERSION 3.20)
project(agent-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(AGENT_BUILD_TESTS "Build tests" ON)
option(AGENT_BUILD_EXAMPLES "Build examples" ON)

# Dependencies
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# nlohmann/json
FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(json)

# asio (standalone, header-only)
FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG asio-1-36-0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(asio)

# Create asio interface library
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE)

# spdlog for logging
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.13.0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(spdlog)

# FTXUI for TUI applications
FetchContent_Declare(
        ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG v5.0.0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# OpenSSL for HTTPS
find_package(OpenSSL REQUIRED)

# Main library
add_library(agent_core
        # Core types
        src/core/types.cpp
        src/core/message.cpp
        src/core/config.cpp
        src/core/json_store.cpp
        src/core/uuid.cpp

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

        # Skill system
        src/skill/skill.cpp
        src/tool/builtin/skill.cpp
)

target_include_directories(agent_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(agent_core PUBLIC
        nlohmann_json::nlohmann_json
        asio
        spdlog::spdlog
        OpenSSL::SSL
        OpenSSL::Crypto
)

# Platform-specific settings
if (APPLE)
    target_link_libraries(agent_core PUBLIC "-framework CoreFoundation" "-framework Security")
endif ()

if (UNIX AND NOT APPLE)
    target_link_libraries(agent_core PUBLIC pthread)
endif ()

# Examples
if (AGENT_BUILD_EXAMPLES)
    add_executable(simple_chat examples/simple_chat.cpp)
    target_link_libraries(simple_chat PRIVATE agent_core)

    add_executable(api-test examples/api_test.cpp)
    target_link_libraries(api-test PRIVATE agent_core)

    add_executable(tool-test examples/tool_test.cpp)
    target_link_libraries(tool-test PRIVATE agent_core)

    add_executable(agent_cli examples/agent_cli.cpp)
    target_link_libraries(agent_cli PRIVATE agent_core ftxui::component)
endif ()

# Tests
if (AGENT_BUILD_TESTS)
    enable_testing()

    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(agent_tests
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
    )

    target_link_libraries(agent_tests PRIVATE
            agent_core
            GTest::gtest
    )

    include(GoogleTest)
    gtest_discover_tests(agent_tests)
endif ()

cmake_minimum_required(VERSION 3.20)

project(agent_runtime_top
    VERSION 0.4.0
    DESCRIPTION "Inference, context, and tool layer for AgentNucleus"
    LANGUAGES NONE)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "agent_runtime_top targets openKylin/openEuler Linux. "
        "Configure it inside Linux, WSL2, or a Linux VM.")
endif()

enable_language(C)
enable_language(CXX)

option(AGENT_RUNTIME_TOP_WITH_LLAMA "Enable the llama.cpp backend" ON)
option(AGENT_RUNTIME_TOP_BUILD_TESTS "Build top-layer tests" ON)
option(AGENT_RUNTIME_TOP_WARNINGS_AS_ERRORS
       "Treat compiler warnings as errors" OFF)

set(AGENT_RUNTIME_TOP_LLAMA_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp"
    CACHE PATH "Path to a llama.cpp source tree")
set(AGENT_RUNTIME_CORE_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../agent-runtime"
    CACHE PATH "Path to the agent-runtime system substrate")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(AGENT_RUNTIME_WITH_LLAMA OFF CACHE BOOL "" FORCE)
set(AGENT_RUNTIME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AGENT_RUNTIME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if(NOT EXISTS "${AGENT_RUNTIME_CORE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "agent-runtime was not found at ${AGENT_RUNTIME_CORE_SOURCE_DIR}")
endif()
add_subdirectory(
    "${AGENT_RUNTIME_CORE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/agent-runtime"
    EXCLUDE_FROM_ALL)

add_library(agent_top_core
    src/context_manager.cpp
    src/llm_llamacpp.c
    src/tool_manager.cpp
    src/top_runtime.cpp)
add_library(agent_runtime_top::core ALIAS agent_top_core)

target_include_directories(agent_top_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_definitions(agent_top_core
    PRIVATE
        _GNU_SOURCE)
target_link_libraries(agent_top_core
    PUBLIC
        agent_runtime::agent_runtime)

if(AGENT_RUNTIME_TOP_WITH_LLAMA)
    if(NOT EXISTS "${AGENT_RUNTIME_TOP_LLAMA_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "llama.cpp was not found at "
            "${AGENT_RUNTIME_TOP_LLAMA_SOURCE_DIR}")
    endif()
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${AGENT_RUNTIME_TOP_LLAMA_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/llama.cpp"
        EXCLUDE_FROM_ALL)
    target_compile_definitions(agent_top_core
        PRIVATE USE_LLAMA_CPP=1
        PUBLIC AGENT_RUNTIME_TOP_WITH_LLAMA=1)
    target_link_libraries(agent_top_core PRIVATE llama)
endif()

if(MSVC)
    target_compile_options(agent_top_core PRIVATE /W4 /permissive-)
    if(AGENT_RUNTIME_TOP_WARNINGS_AS_ERRORS)
        target_compile_options(agent_top_core PRIVATE /WX)
    endif()
else()
    target_compile_options(agent_top_core PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-Wall>
        $<$<COMPILE_LANGUAGE:C>:-Wextra>
        $<$<COMPILE_LANGUAGE:C>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)
    if(AGENT_RUNTIME_TOP_WARNINGS_AS_ERRORS)
        target_compile_options(agent_top_core PRIVATE -Werror)
    endif()
endif()

add_executable(agent_topd apps/agent_topd.cpp)
target_link_libraries(agent_topd PRIVATE agent_runtime_top::core)

add_executable(agent_top_prefix_bench apps/prefix_bench.cpp)
target_link_libraries(agent_top_prefix_bench PRIVATE
    agent_runtime_top::core)

add_executable(agent_top_context_bench apps/context_swap_bench.cpp)
target_link_libraries(agent_top_context_bench PRIVATE
    agent_runtime_top::core)

include(CTest)
if(BUILD_TESTING AND AGENT_RUNTIME_TOP_BUILD_TESTS)
    add_executable(agent_top_context_test tests/context_manager_test.cpp)
    target_link_libraries(agent_top_context_test PRIVATE
        agent_runtime_top::core)
    add_test(NAME top_context_manager COMMAND agent_top_context_test)

    add_executable(agent_top_tool_test tests/tool_manager_test.cpp)
    target_link_libraries(agent_top_tool_test PRIVATE
        agent_runtime_top::core)
    add_test(NAME top_tool_manager COMMAND agent_top_tool_test)

    if(NOT AGENT_RUNTIME_TOP_WITH_LLAMA)
        add_executable(agent_top_runtime_test tests/top_runtime_test.cpp)
        target_link_libraries(agent_top_runtime_test PRIVATE
            agent_runtime_top::core)
        add_test(NAME top_runtime COMMAND agent_top_runtime_test)

        add_executable(agent_top_integration_test
            tests/top_integration_test.cpp)
        target_link_libraries(agent_top_integration_test PRIVATE
            agent_runtime_top::core)
        add_test(NAME top_integration COMMAND agent_top_integration_test)
    endif()
endif()

install(TARGETS
    agent_top_core
    agent_topd
    agent_top_prefix_bench
    agent_top_context_bench
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
install(FILES
    include/context_manager.h
    include/llm.h
    include/tool_manager.h
    include/top_runtime.h
    DESTINATION include)

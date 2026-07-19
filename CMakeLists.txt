cmake_minimum_required(VERSION 3.20)

project(agent_runtime
    VERSION 0.1.0
    DESCRIPTION "OS-oriented runtime for multi-agent workloads"
    LANGUAGES NONE)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "agent-runtime targets openKylin/openEuler Linux. "
        "Configure it inside openKylin, a Linux VM, or WSL2.")
endif()

enable_language(C)
enable_language(CXX)

option(AGENT_RUNTIME_WITH_LLAMA "Build the llama.cpp model engine" OFF)
option(AGENT_RUNTIME_BUILD_TESTS "Build unit tests" ON)
option(AGENT_RUNTIME_BUILD_EXAMPLES "Build examples" ON)
option(AGENT_RUNTIME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(AGENT_RUNTIME_LLAMA_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp"
    CACHE PATH "Path to a llama.cpp source tree")
set(AGENT_RUNTIME_LLAMA_INCLUDE_DIR ""
    CACHE PATH "Path to llama.cpp headers when using a prebuilt library")
set(AGENT_RUNTIME_LLAMA_LIBRARY ""
    CACHE FILEPATH "Path to a prebuilt shared or static llama library")
set(AGENT_RUNTIME_LLAMA_EXTRA_LIBRARIES ""
    CACHE STRING "Additional libraries required by a prebuilt static llama library")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(agent_runtime
    src/cgroup_manager.cpp
    src/control_channel.cpp
    src/control_protocol.cpp
    src/model_engine.cpp
    src/process_executor.cpp
    src/resource_monitor.cpp
    src/runtime.cpp
    src/scheduler.cpp
    src/shared_memory.cpp)

add_library(agent_runtime::agent_runtime ALIAS agent_runtime)

target_include_directories(agent_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_definitions(agent_runtime
    PRIVATE
        $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>)

if(MSVC)
    target_compile_options(agent_runtime PRIVATE /W4 /permissive-)
    if(AGENT_RUNTIME_WARNINGS_AS_ERRORS)
        target_compile_options(agent_runtime PRIVATE /WX)
    endif()
else()
    target_compile_options(agent_runtime PRIVATE -Wall -Wextra -Wpedantic)
    if(AGENT_RUNTIME_WARNINGS_AS_ERRORS)
        target_compile_options(agent_runtime PRIVATE -Werror)
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(agent_runtime PUBLIC Threads::Threads)

if(AGENT_RUNTIME_WITH_LLAMA)
    target_compile_definitions(agent_runtime PUBLIC AGENT_RUNTIME_WITH_LLAMA=1)
    if(AGENT_RUNTIME_LLAMA_LIBRARY)
        if(NOT EXISTS "${AGENT_RUNTIME_LLAMA_LIBRARY}" OR
           NOT EXISTS "${AGENT_RUNTIME_LLAMA_INCLUDE_DIR}/llama.h")
            message(FATAL_ERROR
                "AGENT_RUNTIME_LLAMA_LIBRARY and AGENT_RUNTIME_LLAMA_INCLUDE_DIR "
                "must point to an existing library and llama.h directory.")
        endif()
        add_library(agent_runtime_llama_prebuilt UNKNOWN IMPORTED)
        set_target_properties(agent_runtime_llama_prebuilt PROPERTIES
            IMPORTED_LOCATION "${AGENT_RUNTIME_LLAMA_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${AGENT_RUNTIME_LLAMA_INCLUDE_DIR}")
        target_link_libraries(agent_runtime PRIVATE
            agent_runtime_llama_prebuilt
            ${AGENT_RUNTIME_LLAMA_EXTRA_LIBRARIES})
    else()
        if(NOT EXISTS "${AGENT_RUNTIME_LLAMA_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "llama.cpp was not found at ${AGENT_RUNTIME_LLAMA_SOURCE_DIR}. "
                "Set AGENT_RUNTIME_LLAMA_SOURCE_DIR to a valid source tree.")
        endif()

        set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
        set(GGML_CUDA OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            "${AGENT_RUNTIME_LLAMA_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/llama.cpp"
            EXCLUDE_FROM_ALL)
        target_link_libraries(agent_runtime PRIVATE llama)
    endif()

    add_executable(agent_model_smoke apps/model_smoke.cpp)
    target_link_libraries(agent_model_smoke PRIVATE agent_runtime::agent_runtime)
endif()

add_executable(agentd apps/agentd.cpp)
target_link_libraries(agentd PRIVATE agent_runtime::agent_runtime)

add_executable(agentctl apps/agentctl.cpp)
target_link_libraries(agentctl PRIVATE agent_runtime::agent_runtime)

if(AGENT_RUNTIME_BUILD_EXAMPLES)
    add_executable(agent_pipeline_demo examples/pipeline_demo.cpp)
    target_link_libraries(agent_pipeline_demo PRIVATE agent_runtime::agent_runtime)

    add_executable(agent_ipc_demo examples/ipc_demo.cpp)
    target_link_libraries(agent_ipc_demo PRIVATE agent_runtime::agent_runtime)
endif()

include(CTest)
if(BUILD_TESTING AND AGENT_RUNTIME_BUILD_TESTS)
    add_executable(agent_scheduler_test tests/scheduler_test.cpp)
    target_link_libraries(agent_scheduler_test PRIVATE agent_runtime::agent_runtime)
    add_test(NAME scheduler COMMAND agent_scheduler_test)

    add_executable(agent_shared_memory_test tests/shared_memory_test.cpp)
    target_link_libraries(agent_shared_memory_test PRIVATE agent_runtime::agent_runtime)
    add_test(NAME shared_memory COMMAND agent_shared_memory_test)

    add_executable(agent_process_test tests/process_test.cpp)
    target_link_libraries(agent_process_test PRIVATE agent_runtime::agent_runtime)
    add_test(NAME process COMMAND agent_process_test)

    add_executable(agent_control_protocol_test tests/control_protocol_test.cpp)
    target_link_libraries(agent_control_protocol_test PRIVATE
        agent_runtime::agent_runtime)
    add_test(NAME control_protocol COMMAND agent_control_protocol_test)

    add_executable(agent_control_plane_test tests/control_plane_test.cpp)
    target_link_libraries(agent_control_plane_test PRIVATE
        agent_runtime::agent_runtime)
    add_test(NAME control_plane COMMAND agent_control_plane_test)
endif()

install(TARGETS agent_runtime agentd agentctl
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)

install(DIRECTORY include/ DESTINATION include)

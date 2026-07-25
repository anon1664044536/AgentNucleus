#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "agent_runtime/invocation_channel.h"
#include "agent_runtime/runtime.h"

namespace ar = agent_runtime;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << "check failed at line " << __LINE__ << ": "          \
                      << #condition << '\n';                                    \
            passed = false;                                                     \
        }                                                                       \
    } while (false)

int main() {
    bool passed = true;
    const std::string socket_path =
        "/tmp/agentnucleus-invocation-test-" +
        std::to_string(getpid()) + ".sock";
    std::atomic_bool handler_called{false};

    ar::InvocationServer server(
        [&](const ar::InvocationRequest &request,
            const std::vector<ar::InvocationMappedInput> &inputs,
            const std::function<bool()> &cancel_requested) {
            ar::InvocationHandlerResult result;
            handler_called.store(true);
            if (cancel_requested()) {
                result.status = ar::InvocationStatus::cancelled;
                return result;
            }
            if (request.kind != ar::InvocationKind::model ||
                request.target != "test-model" ||
                request.operation != "generate" ||
                request.payload != R"({"prompt":"answer"})" ||
                inputs.size() != 1 ||
                inputs[0].producer_id != 200) {
                result.status = ar::InvocationStatus::rejected;
                result.message = "unexpected request";
                return result;
            }
            const auto *input = static_cast<const char *>(
                                    inputs[0].region.data()) +
                                inputs[0].reference.offset;
            const std::string input_text(
                input,
                static_cast<std::size_t>(inputs[0].reference.length));
            if (input_text != R"({"intent":"power_query"})") {
                result.status = ar::InvocationStatus::rejected;
                result.message = "unexpected dependency input";
                return result;
            }

            const std::string response = R"({"answer":"ok"})";
            std::string error;
            result.output =
                ar::SharedMemoryRegion::create(response.size(), &error);
            if (!result.output.valid()) {
                result.status = ar::InvocationStatus::failed;
                result.message = error;
                return result;
            }
            std::memcpy(
                result.output.data(), response.data(), response.size());
            result.output_length = response.size();
            result.output_type = ar::SharedDataType::json_utf8;
            result.context_id = 9001;
            result.metrics.execution_time_us = 1200;
            result.metrics.input_tokens = 20;
            result.metrics.output_tokens = 5;
            result.metrics.reused_tokens = 12;
            result.metrics.cache_bytes = 8192;
            result.metrics.cache_hit = true;
            return result;
        },
        socket_path);

    std::string error;
    if (!server.start(&error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::atomic_bool stop_server{false};
    std::thread server_thread(
        [&] { server.serve([&] { return stop_server.load(); }); });

    ar::RuntimeConfig config;
    config.worker_count = 2;
    config.invocation_socket = socket_path;
    ar::AgentRuntime runtime(config);
    if (!runtime.start(&error)) {
        std::cerr << error << '\n';
        stop_server.store(true);
        server_thread.join();
        return 1;
    }

    ar::AgentTaskSpec producer;
    producer.id = 200;
    producer.name = "parse";
    producer.kind = "command";
    producer.command = {
        "/bin/sh", "-c", R"(printf '{"intent":"power_query"}')"};
    producer.resources.timeout = std::chrono::seconds(2);

    ar::AgentTaskSpec consumer;
    consumer.id = 201;
    consumer.name = "generate";
    consumer.kind = "invocation";
    consumer.dependencies = {200};
    consumer.resources.timeout = std::chrono::seconds(2);
    consumer.invocation = ar::InvocationSpec{
        .kind = ar::InvocationKind::model,
        .target = "test-model",
        .operation = "generate",
        .payload = R"({"prompt":"answer"})"};

    CHECK(runtime.submit_batch({producer, consumer}, &error));
    CHECK(runtime.wait_until_idle(std::chrono::seconds(5)));
    const auto consumer_snapshot = runtime.scheduler().snapshot(201);
    CHECK(consumer_snapshot.has_value());
    if (consumer_snapshot.has_value()) {
        CHECK(consumer_snapshot->state == ar::AgentState::completed);
        CHECK(consumer_snapshot->process_id > 0);
        CHECK(consumer_snapshot->context_id == 9001);
        CHECK(consumer_snapshot->metrics.cache_hit);
        CHECK(consumer_snapshot->metrics.reused_tokens == 12);
        CHECK(consumer_snapshot->metrics.output_tokens == 5);
    }

    auto output_handle = runtime.result(201, &error);
    CHECK(output_handle.has_value());
    if (output_handle.has_value()) {
        const ar::SharedBufferRef reference = output_handle->reference;
        auto output = ar::SharedMemoryRegion::map_existing_read_only(
            output_handle->release_descriptor(),
            static_cast<std::size_t>(reference.region_size),
            reference.region_id,
            &error);
        CHECK(output.valid());
        if (output.valid()) {
            const std::string output_text(
                static_cast<const char *>(output.data()) + reference.offset,
                static_cast<std::size_t>(reference.length));
            CHECK(output_text == R"({"answer":"ok"})");
            CHECK(reference.data_type ==
                  static_cast<std::uint32_t>(
                      ar::SharedDataType::json_utf8));
            CHECK((reference.flags & ar::shared_buffer_immutable) != 0);
        }
    }
    CHECK(handler_called.load());

    runtime.stop();
    stop_server.store(true);
    server_thread.join();
    if (!passed) return 1;
    std::cout << "invocation integration tests passed\n";
    return 0;
}

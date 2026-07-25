#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include "agent_runtime/invocation_channel.h"

namespace ar = agent_runtime;

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) {
    stop_requested = 1;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : ar::default_invocation_socket_path();
    ar::InvocationServer server(
        [](const ar::InvocationRequest &request,
           const std::vector<ar::InvocationMappedInput> &inputs,
           const std::function<bool()> &cancel_requested) {
            ar::InvocationHandlerResult result;
            if (cancel_requested()) {
                result.status = ar::InvocationStatus::cancelled;
                return result;
            }
            std::string text = request.payload;
            if (text.empty() && !inputs.empty()) {
                const auto &input = inputs.front();
                text.assign(
                    static_cast<const char *>(input.region.data()) +
                        input.reference.offset,
                    static_cast<std::size_t>(input.reference.length));
            }
            if (text.empty()) text = "{}";

            std::string error;
            result.output =
                ar::SharedMemoryRegion::create(text.size(), &error);
            if (!result.output.valid()) {
                result.status = ar::InvocationStatus::failed;
                result.message = error;
                return result;
            }
            std::memcpy(result.output.data(), text.data(), text.size());
            result.output_length = text.size();
            result.output_type = ar::SharedDataType::json_utf8;
            result.metrics.execution_time_us = 1;
            return result;
        },
        socket_path);

    std::string error;
    if (!server.start(&error)) {
        std::cerr << "failed to start top bridge: " << error << '\n';
        return 1;
    }
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::cout << "top bridge demo listening on " << server.socket_path()
              << '\n';
    return server.serve([] { return stop_requested != 0; });
}

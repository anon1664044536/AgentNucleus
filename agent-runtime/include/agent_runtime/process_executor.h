#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace agent_runtime {

struct ProcessResult {
    bool started{false};
    bool timed_out{false};
    int exit_code{-1};
    std::int64_t process_id{-1};
    std::size_t output_bytes{0};
    bool output_truncated{false};
    std::string error;
};

class ProcessExecutor {
public:
    using StartedCallback =
        std::function<bool(std::int64_t process_id, std::string *error)>;

    ProcessResult run(const std::vector<std::string> &command,
                      std::chrono::milliseconds timeout,
                      const std::atomic_bool *cancel_requested = nullptr,
                      StartedCallback on_started = {},
                      void *output_buffer = nullptr,
                      std::size_t output_capacity = 0) const;
};

}  // namespace agent_runtime

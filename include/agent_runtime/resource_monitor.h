#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "agent_runtime/types.h"

namespace agent_runtime {

struct MemorySnapshot {
    std::uint32_t available_cpu_threads{
        std::numeric_limits<std::uint32_t>::max()};
    std::uint64_t total_bytes{0};
    std::uint64_t available_bytes{0};
    std::uint64_t cgroup_current_bytes{0};
    std::uint64_t cgroup_limit_bytes{0};
    double pressure_avg10{0.0};
};

class ResourceMonitor {
public:
    std::optional<MemorySnapshot> sample(std::string *error = nullptr) const;
};

class MemoryAdmissionController {
public:
    explicit MemoryAdmissionController(
        std::uint64_t safety_margin_bytes = 512ULL * 1024ULL * 1024ULL);

    bool try_reserve(AgentId owner,
                     std::uint64_t bytes,
                     const MemorySnapshot &snapshot,
                     std::string *reason = nullptr);
    void release(AgentId owner);
    std::uint64_t reserved_bytes() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<AgentId, std::uint64_t> reservations_;
    std::uint64_t safety_margin_bytes_;
};

}  // namespace agent_runtime

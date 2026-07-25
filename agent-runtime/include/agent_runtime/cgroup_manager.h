#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "agent_runtime/types.h"

namespace agent_runtime {

struct CgroupLimits {
    std::uint32_t cpu_weight{100};
    std::uint64_t cpu_quota_us{0};
    std::uint64_t cpu_period_us{100000};
    std::uint64_t memory_max_bytes{0};
    std::uint32_t pids_max{32};
};

class CgroupManager {
public:
    explicit CgroupManager(
        std::filesystem::path root = "/sys/fs/cgroup/agent-runtime");

    bool initialize(std::string *error = nullptr);
    bool create_agent_group(AgentId id,
                            const CgroupLimits &limits,
                            std::string *error = nullptr);
    bool attach(AgentId id, std::int64_t process_id, std::string *error = nullptr);
    bool freeze(AgentId id, bool frozen, std::string *error = nullptr);
    bool kill_all(AgentId id, std::string *error = nullptr);
    bool remove_agent_group(AgentId id, std::string *error = nullptr);
    std::filesystem::path group_path(AgentId id) const;

private:
    bool write_value(const std::filesystem::path &path,
                     const std::string &value,
                     std::string *error) const;

    std::filesystem::path root_;
};

}  // namespace agent_runtime

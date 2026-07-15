#include "agent_runtime/cgroup_manager.h"

#include <fstream>
#include <csignal>
#include <chrono>
#include <thread>
#include <utility>
#include <sys/types.h>
#include <unistd.h>

namespace agent_runtime {

CgroupManager::CgroupManager(std::filesystem::path root)
    : root_(std::move(root)) {}

bool CgroupManager::initialize(std::string *error) {
    if (!std::filesystem::exists("/sys/fs/cgroup/cgroup.controllers")) {
        if (error != nullptr) *error = "cgroup v2 is not mounted";
        return false;
    }
    std::error_code status;
    std::filesystem::create_directories(root_, status);
    if (status) {
        if (error != nullptr) {
            *error = "cannot create cgroup root " + root_.string() + ": " +
                     status.message();
        }
        return false;
    }
    return true;
}

bool CgroupManager::write_value(const std::filesystem::path &path,
                                const std::string &value,
                                std::string *error) const {
    std::ofstream output(path);
    output << value;
    if (!output) {
        if (error != nullptr) {
            *error = "cannot write " + path.string();
        }
        return false;
    }
    return true;
}

std::filesystem::path CgroupManager::group_path(AgentId id) const {
    return root_ / ("agent-" + std::to_string(id));
}

bool CgroupManager::create_agent_group(AgentId id,
                                       const CgroupLimits &limits,
                                       std::string *error) {
    const auto path = group_path(id);
    if (std::filesystem::exists(path)) {
        if (error != nullptr) {
            *error = "cgroup already exists: " + path.string();
        }
        return false;
    }
    std::error_code status;
    std::filesystem::create_directory(path, status);
    if (status) {
        if (error != nullptr) *error = "cannot create " + path.string();
        return false;
    }

    const std::uint32_t weight =
        limits.cpu_weight < 1 ? 1 : (limits.cpu_weight > 10000 ? 10000 : limits.cpu_weight);
    const auto write_or_cleanup = [&](const std::filesystem::path &file,
                                      const std::string &value) {
        if (write_value(file, value, error)) return true;
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return false;
    };

    if (!write_or_cleanup(path / "cpu.weight", std::to_string(weight))) return false;

    const std::string cpu_max = limits.cpu_quota_us == 0
        ? "max " + std::to_string(limits.cpu_period_us)
        : std::to_string(limits.cpu_quota_us) + " " +
              std::to_string(limits.cpu_period_us);
    if (!write_or_cleanup(path / "cpu.max", cpu_max)) return false;

    const std::string memory_max = limits.memory_max_bytes == 0
        ? "max"
        : std::to_string(limits.memory_max_bytes);
    if (!write_or_cleanup(path / "memory.max", memory_max)) return false;
    return write_or_cleanup(
        path / "pids.max", std::to_string(limits.pids_max));
}

bool CgroupManager::attach(AgentId id,
                           std::int64_t process_id,
                           std::string *error) {
    return write_value(group_path(id) / "cgroup.procs",
                       std::to_string(process_id),
                       error);
}

bool CgroupManager::freeze(AgentId id, bool frozen, std::string *error) {
    return write_value(group_path(id) / "cgroup.freeze", frozen ? "1" : "0", error);
}

bool CgroupManager::kill_all(AgentId id, std::string *error) {
    const auto path = group_path(id);
    if (std::filesystem::exists(path / "cgroup.kill")) {
        return write_value(path / "cgroup.kill", "1", error);
    }

    std::ifstream processes(path / "cgroup.procs");
    std::int64_t process_id = -1;
    while (processes >> process_id) {
        if (process_id > 0) {
            (void) kill(static_cast<pid_t>(process_id), SIGKILL);
        }
    }
    if (!processes.eof()) {
        if (error != nullptr) *error = "cannot read cgroup.procs";
        return false;
    }
    return true;
}

bool CgroupManager::remove_agent_group(AgentId id, std::string *error) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::error_code status;
    const bool removed = std::filesystem::remove(group_path(id), status);
    if (status || !removed) {
        if (error != nullptr) {
            *error = "cannot remove cgroup for agent " + std::to_string(id);
        }
        return false;
    }
    return true;
}

}  // namespace agent_runtime

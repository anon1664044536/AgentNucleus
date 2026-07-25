#include "agent_runtime/resource_monitor.h"

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <unistd.h>

namespace agent_runtime {
namespace {

bool read_uint_file(const char *path, std::uint64_t *value) {
    std::ifstream input(path);
    std::string text;
    if (!(input >> text) || text == "max") {
        return false;
    }
    try {
        *value = std::stoull(text);
        return true;
    } catch (...) {
        return false;
    }
}

std::filesystem::path current_cgroup_v2_path() {
    std::ifstream membership("/proc/self/cgroup");
    std::string line;
    while (std::getline(membership, line)) {
        constexpr char marker[] = "0::";
        if (line.rfind(marker, 0) != 0) {
            continue;
        }
        std::string relative = line.substr(sizeof(marker) - 1);
        while (!relative.empty() && relative.front() == '/') {
            relative.erase(relative.begin());
        }
        return std::filesystem::path("/sys/fs/cgroup") / relative;
    }
    return "/sys/fs/cgroup";
}

double read_pressure_avg10(const std::filesystem::path &path) {
    std::ifstream pressure(path);
    std::string line;
    if (!std::getline(pressure, line)) {
        return 0.0;
    }
    const std::string marker = "avg10=";
    const std::size_t position = line.find(marker);
    if (position == std::string::npos) {
        return 0.0;
    }
    try {
        return std::stod(line.substr(position + marker.size()));
    } catch (...) {
        return 0.0;
    }
}

}  // namespace

std::optional<MemorySnapshot> ResourceMonitor::sample(std::string *error) const {
    MemorySnapshot snapshot;
    const long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    snapshot.available_cpu_threads = online_cpus > 0
        ? static_cast<std::uint32_t>(online_cpus)
        : 1U;
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo) {
        if (error != nullptr) {
            *error = "cannot open /proc/meminfo";
        }
        return std::nullopt;
    }

    std::string line;
    while (std::getline(meminfo, line)) {
        std::istringstream fields(line);
        std::string key;
        std::uint64_t value_kib = 0;
        if (!(fields >> key >> value_kib)) {
            continue;
        }
        if (key == "MemTotal:") {
            snapshot.total_bytes = value_kib * 1024ULL;
        } else if (key == "MemAvailable:") {
            snapshot.available_bytes = value_kib * 1024ULL;
        }
    }
    if (snapshot.total_bytes == 0 || snapshot.available_bytes == 0) {
        if (error != nullptr) {
            *error = "MemTotal or MemAvailable is missing from /proc/meminfo";
        }
        return std::nullopt;
    }

    const std::filesystem::path cgroup = current_cgroup_v2_path();
    const std::string current_path = (cgroup / "memory.current").string();
    const std::string limit_path = (cgroup / "memory.max").string();
    (void) read_uint_file(current_path.c_str(), &snapshot.cgroup_current_bytes);
    (void) read_uint_file(limit_path.c_str(), &snapshot.cgroup_limit_bytes);

    snapshot.pressure_avg10 = read_pressure_avg10(cgroup / "memory.pressure");
    if (snapshot.pressure_avg10 == 0.0) {
        snapshot.pressure_avg10 = read_pressure_avg10("/proc/pressure/memory");
    }

    if (snapshot.cgroup_limit_bytes > 0) {
        const std::uint64_t cgroup_available =
            snapshot.cgroup_limit_bytes > snapshot.cgroup_current_bytes
                ? snapshot.cgroup_limit_bytes - snapshot.cgroup_current_bytes
                : 0;
        snapshot.available_bytes =
            std::min(snapshot.available_bytes, cgroup_available);
    }
    return snapshot;
}

MemoryAdmissionController::MemoryAdmissionController(
    std::uint64_t safety_margin_bytes)
    : safety_margin_bytes_(safety_margin_bytes) {}

bool MemoryAdmissionController::try_reserve(AgentId owner,
                                            std::uint64_t bytes,
                                            const MemorySnapshot &snapshot,
                                            std::string *reason) {
    std::lock_guard lock(mutex_);
    std::uint64_t reserved = 0;
    for (const auto &[id, reservation] : reservations_) {
        if (id != owner) {
            reserved += reservation;
        }
    }

    const std::uint64_t protected_bytes =
        safety_margin_bytes_ >= snapshot.available_bytes ||
                reserved >= snapshot.available_bytes - safety_margin_bytes_
            ? snapshot.available_bytes
            : safety_margin_bytes_ + reserved;
    const std::uint64_t allocatable = snapshot.available_bytes - protected_bytes;
    if (bytes > allocatable) {
        if (reason != nullptr) {
            *reason = "memory admission rejected: requested=" +
                      std::to_string(bytes) + " allocatable=" +
                      std::to_string(allocatable);
        }
        return false;
    }
    reservations_[owner] = bytes;
    return true;
}

void MemoryAdmissionController::release(AgentId owner) {
    std::lock_guard lock(mutex_);
    reservations_.erase(owner);
}

std::uint64_t MemoryAdmissionController::reserved_bytes() const {
    std::lock_guard lock(mutex_);
    std::uint64_t total = 0;
    for (const auto &[id, bytes] : reservations_) {
        (void) id;
        total += bytes;
    }
    return total;
}

}  // namespace agent_runtime

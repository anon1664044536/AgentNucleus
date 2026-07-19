#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "agent_runtime/types.h"

namespace agent_runtime {

struct SharedBufferRef {
    std::uint64_t region_id{0};
    std::uint64_t region_size{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};
    std::uint32_t data_type{0};
    std::uint32_t flags{0};
    std::uint64_t version{1};
};

static_assert(sizeof(SharedBufferRef) == 48);

enum class SharedDataType : std::uint32_t {
    binary = 0,
    text_utf8 = 1,
    json_utf8 = 2,
};

enum SharedBufferFlags : std::uint32_t {
    shared_buffer_none = 0,
    shared_buffer_truncated = 1U << 0U,
    shared_buffer_immutable = 1U << 1U,
    shared_buffer_stdout_stderr = 1U << 2U,
};

class SharedMemoryRegion {
public:
    SharedMemoryRegion() = default;
    ~SharedMemoryRegion();

    SharedMemoryRegion(const SharedMemoryRegion &) = delete;
    SharedMemoryRegion &operator=(const SharedMemoryRegion &) = delete;
    SharedMemoryRegion(SharedMemoryRegion &&other) noexcept;
    SharedMemoryRegion &operator=(SharedMemoryRegion &&other) noexcept;

    static SharedMemoryRegion create(std::size_t size,
                                     std::string *error = nullptr);
    static SharedMemoryRegion map_existing(int descriptor,
                                           std::size_t size,
                                           std::uint64_t region_id,
                                           std::string *error = nullptr);
    static SharedMemoryRegion map_existing_read_only(
        int descriptor,
        std::size_t size,
        std::uint64_t region_id,
        std::string *error = nullptr);

    bool valid() const noexcept;
    bool resize(std::size_t size, std::string *error = nullptr);
    bool seal_read_only(std::string *error = nullptr);
    void *data() noexcept;
    const void *data() const noexcept;
    std::size_t size() const noexcept;
    int descriptor() const noexcept;
    std::uint64_t region_id() const noexcept;

private:
    void reset() noexcept;

    void *address_{nullptr};
    std::size_t size_{0};
    int descriptor_{-1};
    std::uint64_t region_id_{0};
    bool read_only_{false};
};

class SharedResultHandle {
public:
    SharedResultHandle() = default;
    ~SharedResultHandle();
    SharedResultHandle(const SharedResultHandle &) = delete;
    SharedResultHandle &operator=(const SharedResultHandle &) = delete;
    SharedResultHandle(SharedResultHandle &&other) noexcept;
    SharedResultHandle &operator=(SharedResultHandle &&other) noexcept;

    bool valid() const noexcept;
    int descriptor() const noexcept;
    int release_descriptor() noexcept;

    SharedBufferRef reference;

private:
    friend class AgentResultStore;
    explicit SharedResultHandle(SharedBufferRef value, int descriptor);
    int descriptor_{-1};
};

class AgentResultStore {
public:
    bool publish(AgentId id,
                 SharedMemoryRegion region,
                 std::size_t length,
                 SharedDataType data_type,
                 std::uint32_t flags,
                 std::string *error = nullptr);
    std::optional<SharedResultHandle> acquire(
        AgentId id,
        std::string *error = nullptr) const;
    bool erase(AgentId id);
    void clear();
    std::size_t size() const;

private:
    struct StoredResult {
        SharedMemoryRegion region;
        SharedBufferRef reference;
    };

    mutable std::mutex mutex_;
    std::unordered_map<AgentId, StoredResult> results_;
};

class SharedMemoryChannel {
public:
    static bool create_pair(int descriptors[2], std::string *error = nullptr);
    static bool send(int socket_descriptor,
                     const SharedBufferRef &reference,
                     int memory_descriptor,
                     std::string *error = nullptr);
    static bool receive(int socket_descriptor,
                        SharedBufferRef *reference,
                        int *memory_descriptor,
                        std::string *error = nullptr);
    static void close_descriptor(int descriptor) noexcept;
};

class EventNotifier {
public:
    EventNotifier() = default;
    ~EventNotifier();
    EventNotifier(const EventNotifier &) = delete;
    EventNotifier &operator=(const EventNotifier &) = delete;
    EventNotifier(EventNotifier &&other) noexcept;
    EventNotifier &operator=(EventNotifier &&other) noexcept;

    static EventNotifier create(std::string *error = nullptr);
    bool signal(std::uint64_t value = 1, std::string *error = nullptr) const;
    bool wait(int timeout_ms,
              std::uint64_t *value = nullptr,
              std::string *error = nullptr) const;
    bool valid() const noexcept;
    int descriptor() const noexcept;

private:
    explicit EventNotifier(int descriptor) : descriptor_(descriptor) {}
    int descriptor_{-1};
};

}  // namespace agent_runtime

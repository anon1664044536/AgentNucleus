#include "agent_runtime/shared_memory.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <utility>

#include <sys/mman.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace agent_runtime {
namespace {

std::atomic_uint64_t next_region_id{1};

std::uint64_t make_region_id() {
    const std::uint64_t process = static_cast<std::uint64_t>(getpid());
    const std::uint64_t sequence = next_region_id.fetch_add(1) & 0xffffffffULL;
    return (process << 32U) | sequence;
}

void set_system_error(std::string *error, const char *operation) {
    if (error == nullptr) {
        return;
    }
    *error = std::string(operation) + " failed: " + std::strerror(errno);
}

}  // namespace

SharedMemoryRegion::~SharedMemoryRegion() {
    reset();
}

SharedMemoryRegion::SharedMemoryRegion(SharedMemoryRegion &&other) noexcept {
    *this = std::move(other);
}

SharedMemoryRegion &SharedMemoryRegion::operator=(
    SharedMemoryRegion &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    address_ = std::exchange(other.address_, nullptr);
    size_ = std::exchange(other.size_, 0);
    descriptor_ = std::exchange(other.descriptor_, -1);
    region_id_ = std::exchange(other.region_id_, 0);
    read_only_ = std::exchange(other.read_only_, false);
    return *this;
}

SharedMemoryRegion SharedMemoryRegion::create(std::size_t size,
                                              std::string *error) {
    SharedMemoryRegion region;
    if (size == 0 ||
        size > static_cast<std::uintmax_t>(
                   std::numeric_limits<off_t>::max())) {
        if (error != nullptr) *error = "shared memory size must be non-zero";
        return region;
    }

    const int descriptor =
        memfd_create("agent-runtime", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0) {
        set_system_error(error, "memfd_create");
        return region;
    }
    if (ftruncate(descriptor, static_cast<off_t>(size)) != 0) {
        set_system_error(error, "ftruncate");
        close(descriptor);
        return region;
    }
    void *address = mmap(nullptr,
                         size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         descriptor,
                         0);
    if (address == MAP_FAILED) {
        set_system_error(error, "mmap");
        close(descriptor);
        return region;
    }
    region.descriptor_ = descriptor;
    region.address_ = address;
    region.size_ = size;
    region.region_id_ = make_region_id();
    return region;
}

SharedMemoryRegion SharedMemoryRegion::map_existing(int descriptor,
                                                    std::size_t size,
                                                    std::uint64_t region_id,
                                                    std::string *error) {
    SharedMemoryRegion region;
    if (descriptor < 0 || size == 0) {
        if (error != nullptr) *error = "invalid shared memory descriptor";
        return region;
    }
    struct stat descriptor_status {};
    if (fstat(descriptor, &descriptor_status) != 0 ||
        descriptor_status.st_size < 0 ||
        static_cast<std::uint64_t>(descriptor_status.st_size) < size) {
        if (error != nullptr) *error = "shared memory descriptor is smaller than declared";
        close(descriptor);
        return region;
    }
    void *address = mmap(nullptr,
                         size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         descriptor,
                         0);
    if (address == MAP_FAILED) {
        set_system_error(error, "mmap");
        close(descriptor);
        return region;
    }
    region.address_ = address;
    region.size_ = size;
    region.descriptor_ = descriptor;
    region.region_id_ = region_id;
    return region;
}

SharedMemoryRegion SharedMemoryRegion::map_existing_read_only(
    int descriptor,
    std::size_t size,
    std::uint64_t region_id,
    std::string *error) {
    SharedMemoryRegion region;
    if (descriptor < 0 || size == 0) {
        if (error != nullptr) *error = "invalid shared memory descriptor";
        return region;
    }
    struct stat descriptor_status {};
    if (fstat(descriptor, &descriptor_status) != 0 ||
        descriptor_status.st_size < 0 ||
        static_cast<std::uint64_t>(descriptor_status.st_size) < size) {
        if (error != nullptr) *error = "shared memory descriptor is smaller than declared";
        close(descriptor);
        return region;
    }
    void *address = mmap(nullptr, size, PROT_READ, MAP_SHARED, descriptor, 0);
    if (address == MAP_FAILED) {
        set_system_error(error, "mmap read-only");
        close(descriptor);
        return region;
    }
    region.address_ = address;
    region.size_ = size;
    region.descriptor_ = descriptor;
    region.region_id_ = region_id;
    region.read_only_ = true;
    return region;
}

bool SharedMemoryRegion::valid() const noexcept {
    return address_ != nullptr && size_ > 0;
}

bool SharedMemoryRegion::resize(std::size_t size, std::string *error) {
    if (!valid() || read_only_ || size == 0 ||
        size > static_cast<std::uintmax_t>(
                   std::numeric_limits<off_t>::max())) {
        if (error != nullptr) *error = "invalid shared memory resize";
        return false;
    }
    if (size == size_) return true;
    const std::size_t previous_size = size_;
    if (munmap(address_, size_) != 0) {
        set_system_error(error, "munmap before resize");
        return false;
    }
    address_ = nullptr;
    if (ftruncate(descriptor_, static_cast<off_t>(size)) != 0) {
        address_ = mmap(nullptr,
                        previous_size,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        descriptor_,
                        0);
        if (address_ == MAP_FAILED) address_ = nullptr;
        set_system_error(error, "resize shared memory");
        return false;
    }
    size_ = size;
    address_ = mmap(nullptr,
                    size_,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    descriptor_,
                    0);
    if (address_ == MAP_FAILED) {
        address_ = nullptr;
        set_system_error(error, "remap resized shared memory");
        return false;
    }
    return true;
}

bool SharedMemoryRegion::seal_read_only(std::string *error) {
    if (!valid() || read_only_) {
        if (error != nullptr) *error = "shared memory region is not writable";
        return false;
    }
    if (munmap(address_, size_) != 0) {
        set_system_error(error, "munmap before sealing");
        return false;
    }
    address_ = nullptr;
    constexpr int seals =
        F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (fcntl(descriptor_, F_ADD_SEALS, seals) != 0) {
        set_system_error(error, "seal shared memory");
        return false;
    }
    address_ = mmap(nullptr, size_, PROT_READ, MAP_SHARED, descriptor_, 0);
    if (address_ == MAP_FAILED) {
        address_ = nullptr;
        set_system_error(error, "remap sealed shared memory");
        return false;
    }
    read_only_ = true;
    return true;
}

void *SharedMemoryRegion::data() noexcept {
    return address_;
}

const void *SharedMemoryRegion::data() const noexcept {
    return address_;
}

std::size_t SharedMemoryRegion::size() const noexcept {
    return size_;
}

int SharedMemoryRegion::descriptor() const noexcept {
    return descriptor_;
}

std::uint64_t SharedMemoryRegion::region_id() const noexcept {
    return region_id_;
}

void SharedMemoryRegion::reset() noexcept {
    if (address_ != nullptr && size_ > 0) {
        munmap(address_, size_);
    }
    if (descriptor_ >= 0) {
        close(descriptor_);
    }
    address_ = nullptr;
    size_ = 0;
    descriptor_ = -1;
    region_id_ = 0;
    read_only_ = false;
}

SharedResultHandle::SharedResultHandle(SharedBufferRef value, int descriptor)
    : reference(value), descriptor_(descriptor) {}

SharedResultHandle::~SharedResultHandle() {
    if (descriptor_ >= 0) close(descriptor_);
}

SharedResultHandle::SharedResultHandle(SharedResultHandle &&other) noexcept
    : reference(other.reference),
      descriptor_(std::exchange(other.descriptor_, -1)) {}

SharedResultHandle &SharedResultHandle::operator=(
    SharedResultHandle &&other) noexcept {
    if (this != &other) {
        if (descriptor_ >= 0) close(descriptor_);
        reference = other.reference;
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

bool SharedResultHandle::valid() const noexcept {
    return descriptor_ >= 0 && reference.region_id != 0;
}

int SharedResultHandle::descriptor() const noexcept {
    return descriptor_;
}

int SharedResultHandle::release_descriptor() noexcept {
    return std::exchange(descriptor_, -1);
}

bool AgentResultStore::publish(AgentId id,
                               SharedMemoryRegion region,
                               std::size_t length,
                               SharedDataType data_type,
                               std::uint32_t flags,
                               std::string *error) {
    if (id == kInvalidAgentId || !region.valid() || length > region.size()) {
        if (error != nullptr) *error = "invalid agent result";
        return false;
    }
    const std::size_t retained_size = length == 0 ? 1 : length;
    if (retained_size != region.size() &&
        !region.resize(retained_size, error)) {
        return false;
    }
    if (!region.seal_read_only(error)) return false;
    SharedBufferRef reference{.region_id = region.region_id(),
                              .region_size = region.size(),
                              .offset = 0,
                              .length = length,
                              .data_type = static_cast<std::uint32_t>(data_type),
                              .flags = flags | shared_buffer_immutable,
                              .version = 1};
    std::lock_guard lock(mutex_);
    results_.insert_or_assign(
        id, StoredResult{.region = std::move(region), .reference = reference});
    return true;
}

std::optional<SharedResultHandle> AgentResultStore::acquire(
    AgentId id,
    std::string *error) const {
    std::lock_guard lock(mutex_);
    const auto found = results_.find(id);
    if (found == results_.end()) {
        if (error != nullptr) *error = "agent result is not available";
        return std::nullopt;
    }
    const int descriptor =
        fcntl(found->second.region.descriptor(), F_DUPFD_CLOEXEC, 0);
    if (descriptor < 0) {
        set_system_error(error, "duplicate result descriptor");
        return std::nullopt;
    }
    return SharedResultHandle(found->second.reference, descriptor);
}

bool AgentResultStore::erase(AgentId id) {
    std::lock_guard lock(mutex_);
    return results_.erase(id) != 0;
}

void AgentResultStore::clear() {
    std::lock_guard lock(mutex_);
    results_.clear();
}

std::size_t AgentResultStore::size() const {
    std::lock_guard lock(mutex_);
    return results_.size();
}

bool SharedMemoryChannel::create_pair(int descriptors[2], std::string *error) {
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors) != 0) {
        set_system_error(error, "socketpair");
        return false;
    }
    return true;
}

bool SharedMemoryChannel::send(int socket_descriptor,
                               const SharedBufferRef &reference,
                               int memory_descriptor,
                               std::string *error) {
    if (reference.region_id == 0 || reference.region_size == 0 ||
        reference.offset > reference.region_size ||
        reference.length > reference.region_size - reference.offset ||
        memory_descriptor < 0) {
        if (error != nullptr) *error = "invalid shared buffer reference";
        return false;
    }
    std::atomic_thread_fence(std::memory_order_release);
    iovec payload{};
    payload.iov_base = const_cast<SharedBufferRef *>(&reference);
    payload.iov_len = sizeof(reference);

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &memory_descriptor, sizeof(int));

    if (sendmsg(socket_descriptor, &message, MSG_NOSIGNAL) !=
        static_cast<ssize_t>(sizeof(reference))) {
        set_system_error(error, "sendmsg");
        return false;
    }
    return true;
}

bool SharedMemoryChannel::receive(int socket_descriptor,
                                  SharedBufferRef *reference,
                                  int *memory_descriptor,
                                  std::string *error) {
    if (reference == nullptr || memory_descriptor == nullptr) {
        if (error != nullptr) *error = "receive output pointer is null";
        return false;
    }

    iovec payload{};
    payload.iov_base = reference;
    payload.iov_len = sizeof(*reference);
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};

    msghdr message{};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    const ssize_t received = recvmsg(socket_descriptor, &message, MSG_CMSG_CLOEXEC);
    if (received != static_cast<ssize_t>(sizeof(*reference))) {
        set_system_error(error, "recvmsg");
        return false;
    }
    if ((message.msg_flags & MSG_CTRUNC) != 0) {
        if (error != nullptr) *error = "received descriptor was truncated";
        return false;
    }

    for (cmsghdr *header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
            header->cmsg_len == CMSG_LEN(sizeof(int))) {
            std::memcpy(memory_descriptor, CMSG_DATA(header), sizeof(int));
            if (reference->version != 1 || reference->region_size == 0 ||
                reference->offset > reference->region_size ||
                reference->length > reference->region_size - reference->offset) {
                close(*memory_descriptor);
                *memory_descriptor = -1;
                if (error != nullptr) *error = "invalid shared buffer bounds";
                return false;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            return true;
        }
    }
    if (error != nullptr) *error = "message did not contain a file descriptor";
    return false;
}

void SharedMemoryChannel::close_descriptor(int descriptor) noexcept {
    if (descriptor >= 0) {
        close(descriptor);
    }
}

EventNotifier::~EventNotifier() {
    if (descriptor_ >= 0) close(descriptor_);
}

EventNotifier::EventNotifier(EventNotifier &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

EventNotifier &EventNotifier::operator=(EventNotifier &&other) noexcept {
    if (this != &other) {
        if (descriptor_ >= 0) close(descriptor_);
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

EventNotifier EventNotifier::create(std::string *error) {
    const int descriptor = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (descriptor < 0) {
        set_system_error(error, "eventfd");
        return {};
    }
    return EventNotifier(descriptor);
}

bool EventNotifier::signal(std::uint64_t value, std::string *error) const {
    if (descriptor_ < 0 || value == 0 ||
        value == std::numeric_limits<std::uint64_t>::max()) {
        if (error != nullptr) *error = "invalid eventfd signal";
        return false;
    }
    if (write(descriptor_, &value, sizeof(value)) !=
        static_cast<ssize_t>(sizeof(value))) {
        set_system_error(error, "eventfd write");
        return false;
    }
    return true;
}

bool EventNotifier::wait(int timeout_ms,
                         std::uint64_t *value,
                         std::string *error) const {
    if (descriptor_ < 0) {
        if (error != nullptr) *error = "eventfd is not initialized";
        return false;
    }
    pollfd descriptor{descriptor_, POLLIN, 0};
    const int result = poll(&descriptor, 1, timeout_ms);
    if (result == 0) {
        if (error != nullptr) *error = "eventfd wait timed out";
        return false;
    }
    if (result < 0) {
        set_system_error(error, "eventfd poll");
        return false;
    }
    std::uint64_t observed = 0;
    if (read(descriptor_, &observed, sizeof(observed)) !=
        static_cast<ssize_t>(sizeof(observed))) {
        set_system_error(error, "eventfd read");
        return false;
    }
    if (value != nullptr) *value = observed;
    return true;
}

bool EventNotifier::valid() const noexcept {
    return descriptor_ >= 0;
}

int EventNotifier::descriptor() const noexcept {
    return descriptor_;
}

}  // namespace agent_runtime

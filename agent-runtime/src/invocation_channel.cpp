#include "agent_runtime/invocation_channel.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace agent_runtime {
namespace {

constexpr std::size_t kMaxConcurrentInvocationClients = 64;

void set_error(std::string *error, const std::string &message) {
    if (error != nullptr) *error = message;
}

void set_system_error(std::string *error, const char *operation) {
    if (error != nullptr) {
        *error = std::string(operation) + " failed: " + std::strerror(errno);
    }
}

bool make_socket_address(const std::string &path,
                         sockaddr_un *address,
                         socklen_t *length,
                         std::string *error) {
    if (path.empty() || path.size() >= sizeof(address->sun_path)) {
        set_error(error, "invocation socket path is empty or too long");
        return false;
    }
    std::memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    std::memcpy(address->sun_path, path.c_str(), path.size() + 1);
    *length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return true;
}

void close_descriptors(std::vector<int> *descriptors) {
    for (const int descriptor : *descriptors) {
        if (descriptor >= 0) close(descriptor);
    }
    descriptors->clear();
}

bool send_packet(int socket_descriptor,
                 const std::vector<std::uint8_t> &packet,
                 const std::vector<int> &descriptors,
                 std::string *error) {
    if (packet.empty() || descriptors.size() > kMaxInvocationInputs) {
        set_error(error, "invalid invocation packet attachments");
        return false;
    }
    for (const int descriptor : descriptors) {
        if (descriptor < 0) {
            set_error(error, "invalid invocation descriptor");
            return false;
        }
    }

    iovec payload{const_cast<std::uint8_t *>(packet.data()), packet.size()};
    msghdr message{};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    std::vector<char> control;
    if (!descriptors.empty()) {
        control.resize(CMSG_SPACE(sizeof(int) * descriptors.size()));
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int) * descriptors.size());
        std::memcpy(CMSG_DATA(header),
                    descriptors.data(),
                    sizeof(int) * descriptors.size());
    }
    const ssize_t sent = sendmsg(socket_descriptor, &message, MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(packet.size())) {
        set_system_error(error, "send invocation packet");
        return false;
    }
    return true;
}

bool receive_packet(int socket_descriptor,
                    std::vector<std::uint8_t> *packet,
                    std::vector<int> *descriptors,
                    std::string *error) {
    close_descriptors(descriptors);
    packet->resize(kMaxInvocationMessageSize);
    iovec payload{packet->data(), packet->size()};
    std::vector<char> control(
        CMSG_SPACE(sizeof(int) * kMaxInvocationInputs));
    msghdr message{};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    const ssize_t received =
        recvmsg(socket_descriptor, &message, MSG_CMSG_CLOEXEC);
    if (received <= 0) {
        if (received == 0) {
            set_error(error, "invocation peer closed the connection");
        } else {
            set_system_error(error, "receive invocation packet");
        }
        return false;
    }
    for (cmsghdr *header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(sizeof(int))) {
            continue;
        }
        const std::size_t byte_count =
            header->cmsg_len - CMSG_LEN(0);
        if (byte_count % sizeof(int) != 0) {
            close_descriptors(descriptors);
            set_error(error, "malformed invocation descriptor list");
            return false;
        }
        const std::size_t count = byte_count / sizeof(int);
        if (descriptors->size() + count > kMaxInvocationInputs) {
            close_descriptors(descriptors);
            set_error(error, "too many invocation descriptors");
            return false;
        }
        const auto *received_descriptors =
            reinterpret_cast<const int *>(CMSG_DATA(header));
        descriptors->insert(descriptors->end(),
                            received_descriptors,
                            received_descriptors + count);
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        close_descriptors(descriptors);
        set_error(error, "invocation packet exceeds size limit");
        return false;
    }
    if ((message.msg_flags & MSG_CTRUNC) != 0) {
        close_descriptors(descriptors);
        set_error(error, "invocation descriptors were truncated");
        return false;
    }
    packet->resize(static_cast<std::size_t>(received));
    return true;
}

bool wait_for_response(int descriptor,
                       std::chrono::milliseconds timeout,
                       const std::atomic_bool *cancel_requested,
                       std::string *error) {
    const bool has_deadline = timeout.count() > 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (cancel_requested != nullptr && cancel_requested->load()) {
            set_error(error, "invocation cancelled");
            return false;
        }
        int wait_ms = 50;
        if (has_deadline) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                set_error(error, "invocation timed out");
                return false;
            }
            wait_ms = static_cast<int>(
                std::min<std::int64_t>(wait_ms, remaining.count()));
        }
        pollfd pending{descriptor, POLLIN | POLLHUP, 0};
        const int poll_result = poll(&pending, 1, wait_ms);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            set_system_error(error, "poll invocation response");
            return false;
        }
        if (poll_result == 0) continue;
        if ((pending.revents & POLLIN) != 0) return true;
        set_error(error, "invocation backend disconnected");
        return false;
    }
}

}  // namespace

std::string default_invocation_socket_path() {
    if (const char *runtime_directory = std::getenv("XDG_RUNTIME_DIR")) {
        if (*runtime_directory != '\0') {
            return std::string(runtime_directory) +
                   "/agentnucleus-top.sock";
        }
    }
    return "/tmp/agentnucleus-top-" + std::to_string(getuid()) + ".sock";
}

InvocationClient::InvocationClient(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

bool InvocationClient::invoke(
    const InvocationRequest &request,
    const std::vector<InvocationAttachment> &attachments,
    InvocationCallResult *result,
    std::chrono::milliseconds timeout,
    const std::atomic_bool *cancel_requested,
    std::string *error) const {
    if (error != nullptr) error->clear();
    if (result == nullptr || request.inputs.size() != attachments.size()) {
        set_error(error, "invocation attachment count does not match inputs");
        return false;
    }
    std::vector<int> descriptors;
    descriptors.reserve(attachments.size());
    for (std::size_t index = 0; index < attachments.size(); ++index) {
        const auto &attachment = attachments[index];
        const auto &input = request.inputs[index];
        if (attachment.producer_id != input.producer_id ||
            attachment.reference.region_id != input.reference.region_id ||
            attachment.reference.region_size != input.reference.region_size ||
            attachment.reference.offset != input.reference.offset ||
            attachment.reference.length != input.reference.length ||
            attachment.descriptor < 0) {
            set_error(error, "invocation attachment metadata mismatch");
            return false;
        }
        descriptors.push_back(attachment.descriptor);
    }

    std::vector<std::uint8_t> request_packet;
    if (!encode_invocation_request(request, &request_packet, error)) {
        return false;
    }
    const int socket_descriptor =
        socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socket_descriptor < 0) {
        set_system_error(error, "create invocation socket");
        return false;
    }
    sockaddr_un address{};
    socklen_t address_length = 0;
    if (!make_socket_address(
            socket_path_, &address, &address_length, error) ||
        connect(socket_descriptor,
                reinterpret_cast<const sockaddr *>(&address),
                address_length) != 0) {
        if (error != nullptr && error->empty()) {
            set_system_error(error, "connect invocation backend");
        }
        close(socket_descriptor);
        return false;
    }
    if (!send_packet(
            socket_descriptor, request_packet, descriptors, error) ||
        !wait_for_response(
            socket_descriptor, timeout, cancel_requested, error)) {
        close(socket_descriptor);
        return false;
    }

    std::vector<std::uint8_t> response_packet;
    std::vector<int> response_descriptors;
    if (!receive_packet(socket_descriptor,
                        &response_packet,
                        &response_descriptors,
                        error)) {
        close(socket_descriptor);
        return false;
    }
    close(socket_descriptor);

    InvocationResponse response;
    if (!decode_invocation_response(response_packet.data(),
                                    response_packet.size(),
                                    &response,
                                    error) ||
        response.agent_id != request.agent_id) {
        close_descriptors(&response_descriptors);
        if (error != nullptr && error->empty()) {
            *error = "invocation response does not match request";
        }
        return false;
    }
    if (response.output_available != (response_descriptors.size() == 1)) {
        close_descriptors(&response_descriptors);
        set_error(error, "invocation output descriptor mismatch");
        return false;
    }

    SharedMemoryRegion output;
    if (response.output_available) {
        if (response.output.region_size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            close_descriptors(&response_descriptors);
            set_error(error, "invocation output is too large to map");
            return false;
        }
        const int output_descriptor = response_descriptors.front();
        response_descriptors.clear();
        output = SharedMemoryRegion::map_existing(
            output_descriptor,
            static_cast<std::size_t>(response.output.region_size),
            response.output.region_id,
            error);
        if (!output.valid()) return false;
    }
    result->response = std::move(response);
    result->output = std::move(output);
    return true;
}

InvocationServer::InvocationServer(InvocationHandler handler,
                                   std::string socket_path)
    : handler_(std::move(handler)), socket_path_(std::move(socket_path)) {}

InvocationServer::~InvocationServer() {
    stop();
}

bool InvocationServer::open_socket(std::string *error) {
    sockaddr_un address{};
    socklen_t address_length = 0;
    if (!make_socket_address(
            socket_path_, &address, &address_length, error)) {
        return false;
    }

    struct stat existing {};
    if (lstat(socket_path_.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid()) {
            set_error(error,
                      "refusing to replace invocation socket path");
            return false;
        }
        const int probe =
            socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (probe < 0) {
            set_system_error(error, "probe invocation socket");
            return false;
        }
        const int connect_result = connect(
            probe,
            reinterpret_cast<const sockaddr *>(&address),
            address_length);
        const int connect_error = errno;
        close(probe);
        if (connect_result == 0 ||
            (connect_error != ECONNREFUSED && connect_error != ENOENT)) {
            set_error(error, "invocation socket is already in use");
            return false;
        }
        if (unlink(socket_path_.c_str()) != 0) {
            set_system_error(error, "unlink stale invocation socket");
            return false;
        }
    } else if (errno != ENOENT) {
        set_system_error(error, "inspect invocation socket");
        return false;
    }

    listen_descriptor_ =
        socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listen_descriptor_ < 0) {
        set_system_error(error, "create invocation listener");
        return false;
    }
    if (bind(listen_descriptor_,
             reinterpret_cast<const sockaddr *>(&address),
             address_length) != 0) {
        set_system_error(error, "bind invocation socket");
        close(listen_descriptor_);
        listen_descriptor_ = -1;
        return false;
    }
    owns_socket_ = true;
    if (chmod(socket_path_.c_str(), 0600) != 0 ||
        listen(listen_descriptor_, 32) != 0) {
        set_system_error(error, "configure invocation socket");
        close(listen_descriptor_);
        listen_descriptor_ = -1;
        unlink(socket_path_.c_str());
        owns_socket_ = false;
        return false;
    }
    return true;
}

bool InvocationServer::start(std::string *error) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        set_error(error, "invocation server is already running");
        return false;
    }
    if (!handler_ || !open_socket(error)) {
        running_.store(false);
        if (!handler_) set_error(error, "invocation handler is empty");
        return false;
    }
    return true;
}

int InvocationServer::serve(
    const std::function<bool()> &external_stop) {
    if (listen_descriptor_ < 0) return 1;
    while (running_.load() &&
           !(external_stop && external_stop())) {
        pollfd listener{listen_descriptor_, POLLIN, 0};
        const int poll_result = poll(&listener, 1, 200);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            if (!running_.load()) break;
            return 1;
        }
        if (poll_result == 0) continue;
        const int client =
            accept4(listen_descriptor_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (!running_.load()) break;
            return 1;
        }
        {
            std::lock_guard lock(clients_mutex_);
            if (active_clients_ >= kMaxConcurrentInvocationClients) {
                close(client);
                continue;
            }
            ++active_clients_;
        }
        try {
            std::thread([this, client] {
                handle_client(client);
                close(client);
                {
                    std::lock_guard lock(clients_mutex_);
                    --active_clients_;
                }
                clients_cv_.notify_all();
            }).detach();
        } catch (...) {
            close(client);
            {
                std::lock_guard lock(clients_mutex_);
                --active_clients_;
            }
            clients_cv_.notify_all();
        }
    }
    stop();
    return 0;
}

void InvocationServer::handle_client(int client_descriptor) {
    ucred credentials{};
    socklen_t credentials_size = sizeof(credentials);
    if (getsockopt(client_descriptor,
                   SOL_SOCKET,
                   SO_PEERCRED,
                   &credentials,
                   &credentials_size) != 0 ||
        credentials.uid != geteuid()) {
        return;
    }

    std::vector<std::uint8_t> packet;
    std::vector<int> descriptors;
    std::string error;
    InvocationRequest request;
    if (!receive_packet(
            client_descriptor, &packet, &descriptors, &error) ||
        !decode_invocation_request(
            packet.data(), packet.size(), &request, &error) ||
        descriptors.size() != request.inputs.size()) {
        close_descriptors(&descriptors);
        return;
    }

    std::vector<InvocationMappedInput> inputs;
    inputs.reserve(request.inputs.size());
    for (std::size_t index = 0; index < request.inputs.size(); ++index) {
        const auto &input = request.inputs[index];
        if (input.reference.region_size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            close_descriptors(&descriptors);
            return;
        }
        const int descriptor = descriptors[index];
        descriptors[index] = -1;
        auto region = SharedMemoryRegion::map_existing_read_only(
            descriptor,
            static_cast<std::size_t>(input.reference.region_size),
            input.reference.region_id,
            &error);
        if (!region.valid()) {
            close_descriptors(&descriptors);
            return;
        }
        inputs.push_back({.producer_id = input.producer_id,
                          .reference = input.reference,
                          .region = std::move(region)});
    }
    close_descriptors(&descriptors);

    InvocationHandlerResult handler_result;
    const auto cancel_requested = [client_descriptor] {
        pollfd peer{client_descriptor, POLLHUP | POLLERR, 0};
        const int result = poll(&peer, 1, 0);
        return result > 0 &&
               (peer.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    };
    try {
        handler_result = handler_(request, inputs, cancel_requested);
    } catch (const std::exception &exception) {
        handler_result.status = InvocationStatus::failed;
        handler_result.message = exception.what();
    } catch (...) {
        handler_result.status = InvocationStatus::failed;
        handler_result.message = "invocation handler raised an unknown exception";
    }

    InvocationResponse response;
    response.agent_id = request.agent_id;
    response.process_id = handler_result.process_id >= 0
                              ? handler_result.process_id
                              : static_cast<std::int64_t>(getpid());
    response.status = handler_result.status;
    response.message = std::move(handler_result.message);
    response.context_id = handler_result.context_id;
    response.metrics = handler_result.metrics;
    int output_descriptor = -1;
    if (handler_result.output.valid()) {
        if (handler_result.output_length > handler_result.output.size()) {
            response.status = InvocationStatus::failed;
            response.message =
                "invocation output exceeds its shared region";
        } else {
            response.output_available = true;
            response.output = {
                .region_id = handler_result.output.region_id(),
                .region_size = handler_result.output.size(),
                .offset = 0,
                .length = handler_result.output_length,
                .data_type =
                    static_cast<std::uint32_t>(handler_result.output_type),
                .flags = handler_result.output_flags &
                         ~shared_buffer_immutable,
                .version = 1};
            output_descriptor = handler_result.output.descriptor();
        }
    } else if (handler_result.output_length != 0) {
        response.status = InvocationStatus::failed;
        response.message =
            "invocation handler returned length without output";
    }

    std::vector<std::uint8_t> response_packet;
    if (!encode_invocation_response(
            response, &response_packet, &error)) {
        return;
    }
    std::vector<int> response_descriptors;
    if (response.output_available) {
        output_descriptor = handler_result.output.release_descriptor();
        response_descriptors.push_back(output_descriptor);
    }
    (void) send_packet(client_descriptor,
                       response_packet,
                       response_descriptors,
                       nullptr);
    if (output_descriptor >= 0) close(output_descriptor);
}

void InvocationServer::stop() {
    running_.store(false);
    if (listen_descriptor_ >= 0) {
        close(listen_descriptor_);
        listen_descriptor_ = -1;
    }
    {
        std::unique_lock lock(clients_mutex_);
        clients_cv_.wait(lock, [this] { return active_clients_ == 0; });
    }
    if (owns_socket_) {
        unlink(socket_path_.c_str());
        owns_socket_ = false;
    }
}

const std::string &InvocationServer::socket_path() const noexcept {
    return socket_path_;
}

}  // namespace agent_runtime

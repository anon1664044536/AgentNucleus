#include "agent_runtime/control_channel.h"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace agent_runtime {
namespace {

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
        set_error(error, "control socket path is empty or too long");
        return false;
    }
    std::memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    std::memcpy(address->sun_path, path.c_str(), path.size() + 1);
    *length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return true;
}

bool send_packet(int descriptor,
                 const std::vector<std::uint8_t> &packet,
                 std::string *error) {
    const ssize_t sent = send(descriptor,
                              packet.data(),
                              packet.size(),
                              MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(packet.size())) {
        set_system_error(error, "send");
        return false;
    }
    return true;
}

bool receive_packet(int descriptor,
                    std::vector<std::uint8_t> *packet,
                    std::string *error) {
    packet->resize(kMaxControlMessageSize);
    iovec buffer{packet->data(), packet->size()};
    msghdr message{};
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    const ssize_t received = recvmsg(descriptor, &message, MSG_CMSG_CLOEXEC);
    if (received <= 0) {
        if (received == 0) {
            set_error(error, "control peer closed the connection");
        } else {
            set_system_error(error, "recvmsg");
        }
        return false;
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        set_error(error, "control packet exceeds size limit");
        return false;
    }
    packet->resize(static_cast<std::size_t>(received));
    return true;
}

}  // namespace

std::string default_control_socket_path() {
    if (const char *runtime_directory = std::getenv("XDG_RUNTIME_DIR")) {
        if (*runtime_directory != '\0') {
            return std::string(runtime_directory) + "/agentnucleus.sock";
        }
    }
    return "/tmp/agentnucleus-" + std::to_string(getuid()) + ".sock";
}

ControlClient::ControlClient(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

bool ControlClient::request(const ControlRequest &request,
                            ControlResponse *response,
                            std::string *error) const {
    if (response == nullptr) {
        set_error(error, "control response pointer is null");
        return false;
    }
    std::vector<std::uint8_t> request_packet;
    if (!encode_control_request(request, &request_packet, error)) return false;

    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        set_system_error(error, "socket");
        return false;
    }
    timeval timeout{5, 0};
    (void) setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void) setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    socklen_t address_length = 0;
    if (!make_socket_address(
            socket_path_, &address, &address_length, error)) {
        close(descriptor);
        return false;
    }
    if (connect(descriptor,
                reinterpret_cast<const sockaddr *>(&address),
                address_length) != 0) {
        set_system_error(error, "connect");
        close(descriptor);
        return false;
    }
    if (!send_packet(descriptor, request_packet, error)) {
        close(descriptor);
        return false;
    }
    std::vector<std::uint8_t> response_packet;
    const bool received = receive_packet(descriptor, &response_packet, error);
    close(descriptor);
    return received && decode_control_response(response_packet.data(),
                                               response_packet.size(),
                                               response,
                                               error);
}

AgentDaemon::AgentDaemon(RuntimeConfig runtime_config, std::string socket_path)
    : runtime_(std::move(runtime_config)), socket_path_(std::move(socket_path)) {}

AgentDaemon::~AgentDaemon() {
    stop();
}

bool AgentDaemon::open_socket(std::string *error) {
    sockaddr_un address{};
    socklen_t address_length = 0;
    if (!make_socket_address(socket_path_, &address, &address_length, error)) {
        return false;
    }

    struct stat existing {};
    if (lstat(socket_path_.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid()) {
            set_error(error, "refusing to replace existing control socket path");
            return false;
        }
        const int probe = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (probe < 0) {
            set_system_error(error, "probe control socket");
            return false;
        }
        const int connect_result = connect(
            probe, reinterpret_cast<const sockaddr *>(&address), address_length);
        const int connect_error = errno;
        close(probe);
        if (connect_result == 0 ||
            (connect_error != ECONNREFUSED && connect_error != ENOENT)) {
            set_error(error, "control socket is already in use");
            return false;
        }
        if (unlink(socket_path_.c_str()) != 0) {
            set_system_error(error, "unlink stale socket");
            return false;
        }
    } else if (errno != ENOENT) {
        set_system_error(error, "lstat control socket");
        return false;
    }

    listen_descriptor_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listen_descriptor_ < 0) {
        set_system_error(error, "socket");
        return false;
    }
    if (bind(listen_descriptor_,
             reinterpret_cast<const sockaddr *>(&address),
             address_length) != 0) {
        set_system_error(error, "bind control socket");
        close(listen_descriptor_);
        listen_descriptor_ = -1;
        return false;
    }
    owns_socket_ = true;
    if (chmod(socket_path_.c_str(), 0600) != 0 ||
        listen(listen_descriptor_, 32) != 0) {
        set_system_error(error, "configure control socket");
        close(listen_descriptor_);
        listen_descriptor_ = -1;
        unlink(socket_path_.c_str());
        owns_socket_ = false;
        return false;
    }
    return true;
}

bool AgentDaemon::start(std::string *error) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        set_error(error, "agent daemon is already running");
        return false;
    }
    if (!runtime_.start(error) || !open_socket(error)) {
        running_.store(false);
        runtime_.stop();
        return false;
    }
    return true;
}

int AgentDaemon::serve(const std::function<bool()> &external_stop) {
    if (listen_descriptor_ < 0) return 1;
    while (running_.load() && !(external_stop && external_stop())) {
        pollfd listener{listen_descriptor_, POLLIN, 0};
        const int poll_result = poll(&listener, 1, 200);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (poll_result == 0) continue;

        const int client = accept4(listen_descriptor_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        timeval timeout{5, 0};
        (void) setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void) setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handle_client(client);
        close(client);
    }
    stop();
    return 0;
}

void AgentDaemon::handle_client(int client_descriptor) {
    ucred credentials{};
    socklen_t credential_size = sizeof(credentials);
    if (getsockopt(client_descriptor,
                   SOL_SOCKET,
                   SO_PEERCRED,
                   &credentials,
                   &credential_size) != 0 ||
        credentials.uid != geteuid()) {
        return;
    }

    std::vector<std::uint8_t> packet;
    std::string error;
    ControlResponse response;
    ControlRequest request;
    if (!receive_packet(client_descriptor, &packet, &error) ||
        !decode_control_request(
            packet.data(), packet.size(), &request, &error)) {
        response.message = error.empty() ? "invalid request" : error;
    } else {
        response = dispatch(request);
    }

    std::vector<std::uint8_t> response_packet;
    if (!encode_control_response(response, &response_packet, &error)) {
        ControlResponse fallback;
        fallback.message = error.empty() ? "cannot encode response" : error;
        response_packet.clear();
        if (!encode_control_response(fallback, &response_packet, nullptr)) return;
    }
    (void) send_packet(client_descriptor, response_packet, nullptr);
}

ControlResponse AgentDaemon::dispatch(const ControlRequest &request) {
    ControlResponse response;
    std::string error;
    switch (request.operation) {
        case ControlOperation::ping:
            response.success = true;
            response.message = "agentd is ready";
            break;
        case ControlOperation::submit:
            response.success = runtime_.submit(request.task, &error);
            response.message = response.success ? "agent submitted" : error;
            break;
        case ControlOperation::spawn:
            response.success = runtime_.scheduler().spawn(
                request.target_id, request.task, &error);
            response.message = response.success ? "child agent spawned" : error;
            break;
        case ControlOperation::status: {
            const auto snapshot = runtime_.scheduler().snapshot(request.target_id);
            response.success = snapshot.has_value();
            if (snapshot.has_value()) {
                response.agents.push_back(make_control_info(*snapshot));
            } else {
                response.message = "agent does not exist";
            }
            break;
        }
        case ControlOperation::list:
            response.success = true;
            for (const auto &snapshot : runtime_.scheduler().snapshots()) {
                response.agents.push_back(make_control_info(snapshot));
            }
            break;
        case ControlOperation::cancel:
            response.success = runtime_.cancel(request.target_id, &error);
            response.message = response.success ? "agent cancelled" : error;
            break;
        case ControlOperation::shutdown:
            response.success = true;
            response.message = "agentd is stopping";
            running_.store(false);
            break;
    }
    return response;
}

void AgentDaemon::stop() {
    running_.store(false);
    if (listen_descriptor_ >= 0) {
        close(listen_descriptor_);
        listen_descriptor_ = -1;
    }
    if (owns_socket_) {
        unlink(socket_path_.c_str());
        owns_socket_ = false;
    }
    runtime_.stop();
}

const std::string &AgentDaemon::socket_path() const noexcept {
    return socket_path_;
}

}  // namespace agent_runtime

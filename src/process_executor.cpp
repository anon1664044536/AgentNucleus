#include "agent_runtime/process_executor.h"

#include <chrono>
#include <thread>

#include <csignal>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace agent_runtime {
namespace {

int open_pidfd(pid_t pid) {
#ifdef SYS_pidfd_open
    return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
#else
    (void) pid;
    return -1;
#endif
}

int signal_process(int pidfd, pid_t pid, int signal) {
#ifdef SYS_pidfd_send_signal
    if (pidfd >= 0 && syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0) == 0) {
        return 0;
    }
#else
    (void) pidfd;
#endif
    return kill(pid, signal);
}

}  // namespace

ProcessResult ProcessExecutor::run(
    const std::vector<std::string> &command,
    std::chrono::milliseconds timeout,
    const std::atomic_bool *cancel_requested,
    StartedCallback on_started) const {
    ProcessResult result;
    if (command.empty() || command.front().empty()) {
        result.error = "process command is empty";
        return result;
    }
    if (cancel_requested != nullptr && cancel_requested->load()) {
        result.error = "process was cancelled before start";
        return result;
    }

    std::vector<char *> arguments;
    arguments.reserve(command.size() + 1);
    for (const auto &argument : command) {
        arguments.push_back(const_cast<char *>(argument.c_str()));
    }
    arguments.push_back(nullptr);

    int start_barrier[2] = {-1, -1};
    if (socketpair(AF_UNIX,
                   SOCK_STREAM | SOCK_CLOEXEC,
                   0,
                   start_barrier) != 0) {
        result.error = "failed to create process start barrier";
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(start_barrier[0]);
        close(start_barrier[1]);
        result.error = "fork failed";
        return result;
    }
    if (pid == 0) {
        close(start_barrier[1]);
        char start_token = 0;
        const ssize_t count = read(start_barrier[0], &start_token, 1);
        close(start_barrier[0]);
        if (count != 1 || start_token != 1) {
            _exit(126);
        }
        execvp(arguments.front(), arguments.data());
        _exit(127);
    }

    close(start_barrier[0]);
    result.process_id = pid;
    std::string start_error;
    if (on_started && !on_started(result.process_id, &start_error)) {
        close(start_barrier[1]);
        (void) waitpid(pid, nullptr, 0);
        result.error = start_error.empty() ? "process admission rejected" : start_error;
        return result;
    }
    const char start_token = 1;
    if (send(start_barrier[1], &start_token, 1, MSG_NOSIGNAL) != 1) {
        close(start_barrier[1]);
        (void) kill(pid, SIGKILL);
        (void) waitpid(pid, nullptr, 0);
        result.error = "failed to release process start barrier";
        return result;
    }
    close(start_barrier[1]);
    result.started = true;
    const auto started_at = std::chrono::steady_clock::now();
    const int pidfd = open_pidfd(pid);
    int status = 0;

    for (;;) {
        pid_t waited = 0;
        if (pidfd >= 0) {
            pollfd descriptor{pidfd, POLLIN, 0};
            const int poll_result = poll(&descriptor, 1, 10);
            if (poll_result > 0) {
                waited = waitpid(pid, &status, 0);
            } else if (poll_result < 0 && errno != EINTR) {
                result.error = "poll on pidfd failed";
                close(pidfd);
                return result;
            }
        } else {
            waited = waitpid(pid, &status, WNOHANG);
        }
        if (waited == pid) {
            break;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            result.error = "waitpid failed";
            if (pidfd >= 0) close(pidfd);
            return result;
        }

        const bool cancelled =
            cancel_requested != nullptr && cancel_requested->load();
        const bool expired = timeout.count() > 0 &&
            std::chrono::steady_clock::now() - started_at >= timeout;
        if (cancelled || expired) {
            result.timed_out = expired;
            signal_process(pidfd, pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (waitpid(pid, &status, WNOHANG) == 0) {
                signal_process(pidfd, pid, SIGKILL);
            }
            (void) waitpid(pid, &status, 0);
            break;
        }
        if (pidfd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (pidfd >= 0) {
        close(pidfd);
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

}  // namespace agent_runtime

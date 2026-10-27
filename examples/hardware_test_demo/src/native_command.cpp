#include "native_command.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

namespace hardware_test {

CommandResult run_native_command(const std::vector<std::string> &arguments,
                                 int timeout_ms,
                                 size_t max_output_bytes)
{
    CommandResult result;
    if (arguments.empty() || timeout_ms <= 0 || max_output_bytes == 0U) {
        return result;
    }

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        return result;
    }

    pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return result;
    }

    if (child == 0) {
        close(pipe_fds[0]);
        (void)dup2(pipe_fds[1], STDOUT_FILENO);
        (void)dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);

        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1U);
        for (const std::string &argument : arguments) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    result.started = true;
    close(pipe_fds[1]);

    int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    bool child_reaped = false;
    bool output_closed = false;
    char buffer[4096];

    while (!child_reaped || !output_closed) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            (void)kill(child, SIGKILL);
            if (!child_reaped) {
                (void)waitpid(child, nullptr, 0);
                child_reaped = true;
            }
            break;
        }

        struct pollfd descriptor{};
        descriptor.fd = pipe_fds[0];
        descriptor.events = POLLIN | POLLHUP | POLLERR;
        (void)poll(&descriptor, 1, 100);

        if (!output_closed && (descriptor.revents & (POLLIN | POLLHUP | POLLERR))) {
            while (true) {
                ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
                if (count > 0) {
                    const size_t available = max_output_bytes > result.output.size()
                        ? max_output_bytes - result.output.size()
                        : 0U;
                    if (available > 0U) {
                        result.output.append(buffer,
                                            static_cast<size_t>(count) > available
                                                ? available
                                                : static_cast<size_t>(count));
                    }
                    continue;
                }
                if (count == 0) {
                    output_closed = true;
                }
                break;
            }
        }

        if (!child_reaped) {
            int status = 0;
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) {
                child_reaped = true;
                if (WIFEXITED(status)) {
                    result.exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.exit_code = 128 + WTERMSIG(status);
                }
            }
        }
    }

    if (!child_reaped) {
        int status = 0;
        if (waitpid(child, &status, 0) == child) {
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exit_code = 128 + WTERMSIG(status);
            }
        }
    }

    close(pipe_fds[0]);
    return result;
}

} // namespace hardware_test

#include "tbot/script_runner.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tbot {
namespace {

constexpr std::size_t maximum_output_bytes = 3800;
constexpr int timeout_exit_code = 124;
constexpr int killed_exit_code = 128 + SIGKILL;

std::string trim_output(std::string output) {
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r' || output.back() == ' ' ||
            output.back() == '\t')) {
        output.pop_back();
    }
    return output;
}

}  // namespace

std::string run_script(const std::filesystem::path& script) {
    const auto script_text = script.string();
    if (access(script_text.c_str(), X_OK) != 0) {
        throw std::runtime_error("Script does not exist or is not executable: " + script_text);
    }

    const std::string command = "timeout -s KILL 10 " + script_text + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("Failed to start script: " +
                                 std::string(std::strerror(errno)));
    }

    std::string output;
    bool output_truncated = false;
    char buffer[1024];
    while (const std::size_t bytes_read = std::fread(buffer, 1, sizeof(buffer), pipe)) {
        const auto available = maximum_output_bytes - output.size();
        const auto accepted = std::min(available, bytes_read);
        output.append(buffer, accepted);
        if (accepted < bytes_read) {
            output_truncated = true;
        }
    }

    if (std::ferror(pipe)) {
        const int error = errno;
        pclose(pipe);
        throw std::runtime_error("Failed to read script output: " +
                                 std::string(std::strerror(error)));
    }

    const int status = pclose(pipe);
    if (status < 0) {
        throw std::runtime_error("Failed to wait for script: " +
                                 std::string(std::strerror(errno)));
    }

    output = trim_output(std::move(output));
    if (output_truncated) {
        output += "\n… (output truncated)";
    }

    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if ((WIFEXITED(status) &&
         (exit_code == timeout_exit_code || exit_code == killed_exit_code)) ||
        (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
        throw std::runtime_error("Script exceeded the 10-second timeout");
    }
    if (!WIFEXITED(status) || exit_code != 0) {
        throw std::runtime_error("Script exited with code " + std::to_string(exit_code) +
                                 (output.empty() ? std::string{} : ": " + output));
    }
    if (output.empty()) {
        return "Script completed successfully without output.";
    }
    return output;
}

}  // namespace tbot

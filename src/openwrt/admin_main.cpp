#include "tbot/openwrt/admin.hpp"

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using Json = nlohmann::json;
struct Result { int code; std::string output; };

// Fixed executable/argument lists only. No shell evaluation of request data.
Result run(const char* executable, std::initializer_list<const char*> arguments) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) throw std::runtime_error("Cannot create communication pipe");
    const auto child = fork();
    if (child < 0) { close(pipefd[0]); close(pipefd[1]); throw std::runtime_error("Cannot start service management"); }
    if (child == 0) {
        setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        const int nullfd = open("/dev/null", O_RDWR);
        dup2(nullfd, STDIN_FILENO);
        dup2(nullfd, STDERR_FILENO);
        close(nullfd);
        close(pipefd[0]); close(pipefd[1]);
        std::vector<char*> argv{const_cast<char*>(executable)};
        for (auto argument : arguments) argv.push_back(const_cast<char*>(argument));
        argv.push_back(nullptr);
        execv(executable, argv.data());
        _exit(127);
    }
    setpgid(child, child);
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::string output;
    int status = 0;
    bool done = false;
    while (!done) {
        char buffer[4096];
        ssize_t count;
        while ((count = ::read(pipefd[0], buffer, sizeof buffer)) > 0) {
            if (output.size() + static_cast<std::size_t>(count) <= 65536) output.append(buffer, count);
        }
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) done = true;
        else if (result < 0 && errno != EINTR) { close(pipefd[0]); throw std::runtime_error("Failed to wait for service"); }
        else if (std::chrono::steady_clock::now() >= deadline) {
            kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            close(pipefd[0]);
            throw std::runtime_error("Service management timed out");
        } else {
            pollfd descriptor{pipefd[0], POLLIN, 0};
            poll(&descriptor, 1, 20);
        }
    }
    // Drain output written just before child exit.
    char buffer[4096];
    ssize_t count;
    while ((count = ::read(pipefd[0], buffer, sizeof buffer)) > 0)
        if (output.size() + static_cast<std::size_t>(count) <= 65536) output.append(buffer, count);
    close(pipefd[0]);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

Json status() {
    const auto result = run("/bin/ubus", {"call", "service", "list", "{\"name\":\"tbot-wrt\"}"});
    if (result.code != 0) throw std::runtime_error("Cannot determine service status through ubus");
    const auto services = Json::parse(result.output);
    bool running = false;
    Json pid = nullptr;
    if (services.contains("tbot-wrt"))
        for (const auto& instance : services.at("tbot-wrt").value("instances", Json::object())) {
            running = running || instance.value("running", false);
            if (instance.value("running", false) && instance.contains("pid")) pid = instance.at("pid");
        }
    const auto enabled = run("/etc/init.d/tbot-wrt", {"enabled"});
    if (enabled.code != 0 && enabled.code != 1) throw std::runtime_error("Cannot determine service autostart status");
    return {{"running", running}, {"enabled", enabled.code == 0}, {"pid", pid}};
}

Json service(const std::string& action) {
    if (action != "start" && action != "stop" && action != "restart" && action != "enable" && action != "disable")
        throw std::runtime_error("Service action not allowed");
    const auto result = run("/etc/init.d/tbot-wrt", {action.c_str()});
    if (result.code != 0) throw std::runtime_error("Service action failed; check the system log");
    if (action == "start" || action == "restart") std::this_thread::sleep_for(std::chrono::seconds(1));
    auto state = status();
    if ((action == "start" || action == "restart") && !state.at("running").get<bool>())
        throw std::runtime_error("Service is not running after startup; check the configuration and system log");
    if (action == "stop" && state.at("running").get<bool>())
        throw std::runtime_error("Service did not stop");
    if ((action == "enable" && !state.at("enabled").get<bool>()) ||
        (action == "disable" && state.at("enabled").get<bool>()))
        throw std::runtime_error("Autostart setting did not change");
    return state;
}
}

int main(int argc, char** argv) {
    // RPC always uses the fixed router path; test code uses AdminConfig directly.
    try {
        if (argc != 2) throw std::runtime_error("Expected one RPC method");
        std::string text;
        char c;
        while (std::cin.get(c)) {
            if (text.size() >= 1024 * 1024) throw std::runtime_error("Request is too large");
            text += c;
        }
        const auto request = Json::parse(text.empty() ? "{}" : text);
        const std::string method(argv[1]);
        tbot_wrt::AdminConfig config("/etc/tbot-wrt/config.json");
        Json result;
        if (method == "load" || method == "recover") result = config.load();
        else if (method == "script") result = config.script(request.at("command").get<std::string>(), request.at("revision").get<std::string>());
        else if (method == "test_script") result = config.test_script(request.at("command").get<std::string>(), request.at("revision").get<std::string>());
        else if (method == "save") {
            const auto& settings = request.at("request");
            const bool apply = settings.value("apply", false);
            result = config.save(settings);
            if (apply) {
                try { result["service"] = service("restart"); }
                catch (const std::exception& error) { result["service_error"] = error.what(); }
            }
        } else if (method == "generate") result = tbot_wrt::AdminConfig::generate(request.at("user_id").get<std::string>());
        else if (method == "status") result = status();
        else if (method == "service") result = service(request.at("action").get<std::string>());
        else throw std::runtime_error("Unknown RPC method");
        std::cout << Json({{"ok", true}, {"data", result}}).dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
    } catch (const nlohmann::json::exception&) {
        std::cout << Json({{"ok", false}, {"error", "Missing or incorrectly typed request or configuration field"}}).dump() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cout << Json({{"ok", false}, {"error", error.what()}}).dump() << '\n';
        return 1;
    }
}

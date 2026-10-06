#include "tbot/openwrt/admin.hpp"
#include "tbot/config.hpp"
#include "tbot/qr_code.hpp"
extern "C" {
#include "google_pam/sha1.h"
}

#include <charconv>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <set>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <sys/wait.h>
#include <signal.h>

namespace tbot_wrt {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr std::size_t limit = 256 * 1024;
constexpr std::size_t journal_limit = 16 * limit + 4096;
constexpr std::size_t script_limit = 64 * 1024;

[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }

class Lock {
    int fd_;
public:
    explicit Lock(const fs::path& path) : fd_(open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)) {
        if (fd_ < 0) fail("Cannot lock configuration");
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            close(fd_);
            fail("Another request is saving the configuration; try again");
        }
    }
    ~Lock() { close(fd_); }
};

std::string read(const fs::path& path) {
    if (!fs::is_regular_file(path) || fs::is_symlink(fs::symlink_status(path)))
        fail("Configuration must be a regular file, not a symbolic link");
    std::ifstream input(path, std::ios::binary);
    if (!input) fail("Cannot read configuration");
    std::string value;
    char buffer[4096];
    while (input.read(buffer, sizeof buffer) || input.gcount()) {
        value.append(buffer, static_cast<std::size_t>(input.gcount()));
        if (value.size() > limit) fail("Configuration exceeds 256 KiB");
    }
    if (input.bad()) fail("Failed to read configuration");
    return value;
}

void sync_directory(const fs::path& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) fail("Cannot open configuration directory");
    const int result = fsync(fd);
    close(fd);
    if (result != 0) fail("Failed to synchronize configuration directory");
}

void write_atomic(const fs::path& path, const std::string& value, const Json& metadata = nullptr) {
    if (value.size() > journal_limit) fail("Data to save is too large");
    std::string temporary = path.string() + ".XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) fail("Cannot create temporary configuration file");
    bool closed = false;
    try {
        if (fchmod(fd, 0600) != 0) fail("Cannot set configuration permissions");
        std::size_t offset = 0;
        while (offset < value.size()) {
            const auto count = ::write(fd, value.data() + offset, value.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) fail("Failed to write configuration");
            offset += static_cast<std::size_t>(count);
        }
        if (!metadata.is_null()) {
            if (fchown(fd, metadata.at("uid").get<uid_t>(), metadata.at("gid").get<gid_t>()) != 0 ||
                fchmod(fd, metadata.at("mode").get<mode_t>()) != 0)
                fail("Cannot preserve script ownership and permissions");
        }
        if (fsync(fd) != 0) fail("Failed to synchronize configuration");
        const int result = close(fd);
        closed = true;
        if (result != 0) fail("Failed to close configuration file");
        fs::rename(temporary, path);
        sync_directory(path.parent_path());
    } catch (...) {
        if (!closed) close(fd);
        unlink(temporary.c_str());
        throw;
    }
}

Json parse(const std::string& text) {
    auto result = Json::parse(text, nullptr, false);
    if (result.is_discarded() || !result.is_object()) fail("Invalid configuration JSON; fix the file over SSH");
    return result;
}

fs::path commands_path(const fs::path& config, const Json& document) {
    fs::path path = document.value("commands_file", std::string("commands.json"));
    if (path.is_relative()) path = config.parent_path() / path;
    path = fs::absolute(path).lexically_normal();
    if (path == config || path == config.string() + ".luci-lock" ||
        path == config.string() + ".luci-pending" || path == config.string() + ".luci-backup")
        fail("commands_file conflicts with the main configuration or administration files");
    return path;
}

std::string revision(const std::string& config, const fs::path& path, const std::string& commands) {
    // Content fingerprint, not an authentication primitive. Never expose raw secret-bearing data.
    SHA1_INFO state;
    sha1_init(&state);
    for (const auto& text : {config, path.string(), commands}) {
        sha1_update(&state, reinterpret_cast<const std::uint8_t*>(text.data()), static_cast<int>(text.size()));
        const std::uint8_t separator = 0;
        sha1_update(&state, &separator, 1);
    }
    std::uint8_t digest[20];
    sha1_final(&state, digest);
    std::string result;
    for (auto byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}

fs::path editable_path(const fs::path& config, fs::path path) {
    const auto root = (config.parent_path() / "scripts").lexically_normal();
    path = fs::absolute(path).lexically_normal();
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative == "." || relative.is_absolute() || *relative.begin() == "..")
        fail("The editor only supports existing scripts in /etc/tbot-wrt/scripts");
    fs::path current;
    for (const auto& part : path) {
        current /= part;
        if (fs::is_symlink(fs::symlink_status(current))) fail("The editor does not allow symbolic links");
    }
    if (!fs::is_regular_file(path)) fail("Script is not an existing regular file");
    return path;
}

Json script_metadata(const fs::path& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || !(info.st_mode & 0111))
        fail("Script must be a regular executable file");
    return {{"mode", info.st_mode & 07777}, {"uid", info.st_uid}, {"gid", info.st_gid}};
}

void shell_text(const std::string& content) {
    auto header = content.substr(0, content.find('\n'));
    if (!header.empty() && header.back() == '\r') header.pop_back();
    if (content.size() > script_limit || content.find('\0') != std::string::npos ||
        (header != "#!/bin/sh" && header != "#!/bin/ash"))
        fail("The editor requires a text script starting with #!/bin/sh or #!/bin/ash, at most 64 KiB");
}

void check_shell_syntax(const std::string& content) {
    shell_text(content);
    char name[] = "/tmp/tbot-script-check-XXXXXX";
    const int fd = mkstemp(name);
    if (fd < 0) fail("Cannot prepare script syntax check");
    unlink(name); // Anonymous, private input; never an executable script path.
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto n = ::write(fd, content.data() + offset, content.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); fail("Cannot prepare script syntax check"); }
        offset += static_cast<std::size_t>(n);
    }
    lseek(fd, 0, SEEK_SET);
    const auto child = fork();
    if (child == 0) {
        dup2(fd, STDIN_FILENO); close(fd);
        const int sink = open("/dev/null", O_WRONLY);
        dup2(sink, STDOUT_FILENO); dup2(sink, STDERR_FILENO); close(sink);
        execl("/bin/sh", "sh", "-n", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fd);
    if (child < 0) fail("Cannot start syntax check");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = 0;
    for (;;) {
        const auto done = waitpid(child, &status, WNOHANG);
        if (done == child) break;
        if (done < 0 && errno != EINTR) fail("Syntax check failed");
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            fail("Syntax check timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("Script has invalid shell syntax; nothing was saved");
}

std::int64_t id(const Json& value, bool positive) {
    if (!value.is_string()) fail("Telegram ID must be a decimal string");
    const auto text = value.get<std::string>();
    std::int64_t number;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc{} || end != text.data() + text.size() || number == 0 ||
        (positive && number < 0)) fail("Invalid Telegram ID");
    return number;
}

Json ids(const Json& values, bool positive) {
    if (!values.is_array()) fail("ID list must be an array");
    Json result = Json::array();
    std::set<std::int64_t> seen;
    for (const auto& value : values) {
        const auto number = id(value, positive);
        if (!seen.insert(number).second) fail("Duplicate Telegram ID");
        result.push_back(number);
    }
    return result;
}

Json string_ids(const Json& values) {
    Json result = Json::array();
    for (const auto& value : values) result.push_back(std::to_string(value.get<std::int64_t>()));
    return result;
}

void recover(const fs::path& config) {
    const auto pending = config.string() + ".luci-pending";
    if (!fs::exists(pending)) return;
    // Journal can be larger than an individual document; read using a bounded stream.
    std::ifstream input(pending, std::ios::binary);
    std::string text;
    char c;
    while (input.get(c)) {
        text += c;
        if (text.size() > journal_limit) fail("Corrupted transaction journal");
    }
    const auto journal = parse(text);
    const auto before = journal.at("config_before").get<std::string>();
    const auto target = commands_path(config, parse(before));
    const auto current_config = read(config);
    const auto current_commands = read(target);
    if ((current_config != before && current_config != journal.at("config_after").get<std::string>()) ||
        (current_commands != journal.at("commands_before").get<std::string>() &&
         current_commands != journal.at("commands_after").get<std::string>()))
        fail("Incomplete save and concurrent change over SSH; restore the backup manually");
    const auto scripts = journal.value("scripts", Json::array());
    // Validate the entire recovery set before restoring any member.
    for (const auto& script : scripts) {
        const auto path = editable_path(config, script.at("path").get<std::string>());
        const auto content = read(path);
        if ((content != script.at("before").get<std::string>() && content != script.at("after").get<std::string>()) ||
            script_metadata(path) != script.at("metadata"))
            fail("Incomplete save and concurrent script change; restore the backup manually");
    }
    for (const auto& script : scripts)
        write_atomic(script.at("path").get<std::string>(), script.at("before").get<std::string>(), script.at("metadata"));
    write_atomic(target, journal.at("commands_before").get<std::string>());
    write_atomic(config, before);
    fs::remove(pending);
    sync_directory(config.parent_path());
}

struct Snapshot {
    std::string config_text, commands_text;
    Json config, commands;
    fs::path target;
    explicit Snapshot(const fs::path& path) {
        config_text = read(path);
        config = parse(config_text);
        target = commands_path(path, config);
        commands_text = read(target);
        commands = parse(commands_text);
    }
    std::string version() const { return revision(config_text, target, commands_text); }
};

fs::path command_script(const fs::path& config, const Snapshot& snapshot, const std::string& name) {
    for (const auto& command : snapshot.commands.at("commands")) {
        if (command.at("name") != name) continue;
        fs::path path = command.at("script").get<std::string>();
        if (path.is_relative()) path = snapshot.target.parent_path() / path;
        return editable_path(config, path);
    }
    fail("Saved command does not exist; save the configuration first");
}

Json public_view(const Snapshot& snapshot) {
    const auto& telegram = snapshot.config.at("telegram");
    const auto& notifications = snapshot.config.at("notifications");
    const auto http = notifications.value("http", Json::object());
    Json users = Json::array(), commands = Json::array();
    for (const auto& user : telegram.value("two_factor_users", Json::array()))
        users.push_back({{"user_id", std::to_string(user.at("user_id").get<std::int64_t>())},
                         {"secret_action", "keep"}});
    for (const auto& command : snapshot.commands.at("commands")) {
        fs::path script_path = command.at("script").get<std::string>();
        if (script_path.is_relative()) script_path = snapshot.target.parent_path() / script_path;
        Json visible = {{"original_name", command.at("name")}, {"name", command.at("name")},
                        {"description", command.at("description")}, {"script", command.at("script")},
                        {"script_path", script_path.lexically_normal().string()},
                        {"requires_2fa", command.value("requires_2fa", false)},
                        {"enabled", command.value("enabled", true)}};
        if (command.contains("button"))
            visible["button"] = {{"text", command.at("button").at("text")},
                                 {"row", command.at("button").value("row", 0)}};
        commands.push_back(visible);
    }
    return {{"revision", snapshot.version()},
            {"token_present", !telegram.value("token", std::string()).empty()},
            {"allowed_user_ids", string_ids(telegram.at("allowed_user_ids"))},
            {"poll_timeout_seconds", telegram.value("poll_timeout_seconds", 30)},
            {"chat_ids", string_ids(notifications.at("chat_ids"))},
            {"socket_path", notifications.value("socket_path", "/tmp/tbot-wrt.sock")},
            {"http", {{"enabled", http.value("enabled", false)},
                      {"listen_address", http.value("listen_address", "127.0.0.1")},
                      {"port", http.value("port", 8088)},
                      {"username", http.value("username", "")},
                      {"password_present", !http.value("password", std::string()).empty()},
                      {"password_action", "keep"}}},
            {"commands_file", snapshot.config.value("commands_file", "commands.json")},
            {"two_factor_users", users}, {"commands", commands}};
}
}

AdminConfig::AdminConfig(fs::path config) : config_(fs::absolute(config).lexically_normal()) {}

Json AdminConfig::load() {
    Lock lock(config_.string() + ".luci-lock");
    recover(config_);
    const Snapshot snapshot(config_);
    auto result = public_view(snapshot);
    try { (void)tbot::TbotConfig::from_json(config_, snapshot.config, snapshot.commands); }
    catch (const std::exception&) {
        result["validation_warning"] = "Configuration is not valid yet. Complete and save it before starting.";
    }
    return result;
}

Json AdminConfig::script(const std::string& command, const std::string& config_revision) {
    Lock lock(config_.string() + ".luci-lock");
    recover(config_);
    const Snapshot snapshot(config_);
    if (snapshot.version() != config_revision) fail("Configuration has changed. Reload the page.");
    const auto path = command_script(config_, snapshot, command);
    const auto content = read(path);
    shell_text(content);
    const auto metadata = script_metadata(path);
    return {{"path", path.string()}, {"content", content},
            {"revision", revision(content, path, metadata.dump())}};
}

Json AdminConfig::test_script(const std::string& command, const std::string& config_revision) {
    Lock lock(config_.string() + ".luci-lock");
    recover(config_);
    const Snapshot snapshot(config_);
    if (snapshot.version() != config_revision) fail("Configuration has changed. Reload the page.");
    const auto path = command_script(config_, snapshot, command);
    shell_text(read(path));
    (void)script_metadata(path);
    int output_pipe[2];
    if (pipe2(output_pipe, O_CLOEXEC) != 0) fail("Cannot create pipe for script output");
    const auto child = fork();
    if (child < 0) { close(output_pipe[0]); close(output_pipe[1]); fail("Cannot start script"); }
    if (child == 0) {
        setpgid(0, 0);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        const int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) _exit(126);
        close(nullfd); close(output_pipe[0]); close(output_pipe[1]);
        chdir("/");
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    setpgid(child, child);
    close(output_pipe[1]);
    fcntl(output_pipe[0], F_SETFL, O_NONBLOCK);
    std::string output;
    bool truncated = false, timed_out = false;
    const auto drain = [&] {
        char buffer[4096];
        // Bound each pass so continuous output cannot defeat the deadline.
        for (int i = 0; i < 16; ++i) {
            const auto count = ::read(output_pipe[0], buffer, sizeof buffer);
            if (count <= 0) break;
            const auto take = std::min(static_cast<std::size_t>(count), 65536 - output.size());
            output.append(buffer, take);
            truncated = truncated || take < static_cast<std::size_t>(count);
        }
    };
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
        drain();
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) {
            kill(-child, SIGKILL); close(output_pipe[0]); fail("Failed to wait for script");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(-child, SIGKILL); // Do not leave background children from a test running.
    drain(); close(output_pipe[0]);
    return {{"output", output}, {"exit_code", WIFEXITED(status) ? WEXITSTATUS(status) : -1},
            {"timed_out", timed_out}, {"truncated", truncated}};
}

Json AdminConfig::save(const Json& request, const std::function<void()>& after_first_write,
                       const std::function<void()>& after_script_write) {
    Lock lock(config_.string() + ".luci-lock");
    recover(config_);
    const Snapshot snapshot(config_);
    if (request.at("revision") != snapshot.version()) fail("Configuration has changed. Reload the page.");
    auto config = snapshot.config;
    auto commands = snapshot.commands;
    auto& telegram = config["telegram"];
    const auto token_action = request.at("token_action").get<std::string>();
    if (token_action == "replace") telegram["token"] = request.at("token").get<std::string>();
    else if (token_action != "keep") fail("Invalid token operation");
    telegram["allowed_user_ids"] = ids(request.at("allowed_user_ids"), true);
    telegram["poll_timeout_seconds"] = request.at("poll_timeout_seconds");
    config["notifications"]["chat_ids"] = ids(request.at("chat_ids"), false);
    config["notifications"]["socket_path"] = request.at("socket_path").get<std::string>();
    if (request.contains("http")) {
        const auto& source = request.at("http");
        auto& target = config["notifications"]["http"];
        if (target.is_null()) target = Json::object();
        for (const auto* key : {"enabled", "listen_address", "port", "username"})
            target[key] = source.at(key);
        const auto action = source.at("password_action").get<std::string>();
        if (action == "replace") target["password"] = source.at("password").get<std::string>();
        else if (action != "keep") fail("Invalid HTTP password operation");
    }
    if (!request.at("two_factor_users").is_array() || !request.at("commands").is_array())
        fail("Users and commands must be arrays");
    Json users = Json::array();
    for (const auto& item : request.at("two_factor_users")) {
        const auto number = id(item.at("user_id"), true);
        Json user = {{"user_id", number}};
        for (const auto& old : telegram.value("two_factor_users", Json::array()))
            if (old.at("user_id") == number) user = old;
        const auto action = item.at("secret_action").get<std::string>();
        if (action == "replace") user["secret"] = item.at("secret").get<std::string>();
        else if (action != "keep" || !user.contains("secret")) fail("Missing new 2FA secret");
        users.push_back(user);
    }
    telegram["two_factor_users"] = users; // Omitted users are explicitly removed by the form.
    Json items = Json::array();
    std::set<std::string> originals;
    for (const auto& item : request.at("commands")) {
        Json command = Json::object();
        const auto original = item.value("original_name", std::string());
        if (!original.empty()) {
            if (!originals.insert(original).second) fail("Duplicate original command");
            bool found = false;
            for (const auto& old : snapshot.commands.at("commands"))
                if (old.at("name") == original) { command = old; found = true; break; }
            if (!found) fail("Original command does not exist");
        }
        for (const auto* key : {"name", "description", "script"}) command[key] = item.at(key).get<std::string>();
        command["requires_2fa"] = item.at("requires_2fa").get<bool>();
        command["enabled"] = item.contains("enabled")
            ? item.at("enabled").get<bool>() : command.value("enabled", true);
        if (item.contains("button")) {
            command["button"]["text"] = item.at("button").at("text").get<std::string>();
            command["button"]["row"] = item.at("button").at("row");
        } else command.erase("button");
        items.push_back(command);
    }
    commands["commands"] = items;
    (void)tbot::TbotConfig::from_json(config_, config, commands);
    const auto next_config = config.dump(2) + "\n";
    const auto next_commands = commands.dump(2) + "\n";
    if (next_config.size() > limit || next_commands.size() > limit) fail("Configuration exceeds 256 KiB");
    const auto edits = request.value("scripts", Json::array());
    if (!edits.is_array() || edits.size() > 32) fail("Invalid list of modified scripts");
    Json script_changes = Json::array();
    std::set<fs::path> script_paths;
    std::size_t total_size = 0;
    for (const auto& edit : edits) {
        const auto name = edit.at("command").get<std::string>();
        const auto path = command_script(config_, snapshot, name);
        bool still_assigned = false;
        for (const auto& item : request.at("commands")) {
            if (item.value("original_name", std::string()) != name) continue;
            fs::path candidate = item.at("script").get<std::string>();
            if (candidate.is_relative()) candidate = snapshot.target.parent_path() / candidate;
            still_assigned = candidate.lexically_normal() == path;
        }
        if (!still_assigned) fail("A modified script had its command removed or its path changed");
        if (!script_paths.insert(path).second) fail("A script can only be modified once per request");
        const auto before = read(path);
        shell_text(before);
        const auto metadata = script_metadata(path);
        if (edit.at("revision") != revision(before, path, metadata.dump())) fail("Script has changed; reload it");
        const auto after = edit.at("content").get<std::string>();
        total_size += after.size();
        if (total_size > 128 * 1024) fail("Modified scripts exceed 128 KiB in total");
        check_shell_syntax(after);
        script_changes.push_back({{"path", path.string()}, {"before", before}, {"after", after}, {"metadata", metadata}});
    }
    // Recheck immediately before writing, including changes made outside our lock (SSH).
    if (Snapshot(config_).version() != snapshot.version()) fail("Configuration changed while saving");
    for (const auto& change : script_changes) {
        const auto path = editable_path(config_, change.at("path").get<std::string>());
        if (read(path) != change.at("before").get<std::string>() || script_metadata(path) != change.at("metadata"))
            fail("Script changed while saving");
    }
    const Json journal = {{"config_before", snapshot.config_text}, {"commands_before", snapshot.commands_text},
                          {"config_after", next_config}, {"commands_after", next_commands}, {"scripts", script_changes}};
    const auto pending = config_.string() + ".luci-pending";
    write_atomic(config_.string() + ".luci-backup", journal.dump());
    write_atomic(pending, journal.dump());
    try {
        write_atomic(snapshot.target, next_commands);
        if (after_first_write) after_first_write();
        for (const auto& change : script_changes) {
            write_atomic(change.at("path").get<std::string>(), change.at("after").get<std::string>(), change.at("metadata"));
            if (after_script_write) after_script_write();
        }
        write_atomic(config_, next_config);
    } catch (...) {
        recover(config_);
        throw;
    }
    fs::remove(pending);
    sync_directory(config_.parent_path());
    return public_view(Snapshot(config_));
}

Json AdminConfig::generate(const std::string& user_id) {
    const auto number = id(user_id, true);
    const auto secret = tbot::TwoFactor::generate_secret();
    const auto uri = "otpauth://totp/tbot-wrt:" + std::to_string(number) + "?secret=" + secret +
                     "&issuer=tbot-wrt&algorithm=SHA1&digits=6&period=30";
    tbot::QrCode qr(uri);
    Json rows = Json::array();
    for (int y = 0; y < qr.size(); ++y) {
        std::string row;
        for (int x = 0; x < qr.size(); ++x) row += qr.module(x, y) ? '1' : '0';
        rows.push_back(row);
    }
    return {{"user_id", std::to_string(number)}, {"secret", secret}, {"uri", uri}, {"qr", rows}};
}
}

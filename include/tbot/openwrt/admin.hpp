#pragma once

#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

namespace tbot_wrt {
// Paths and fault hooks are supplied by local code only, never by RPC requests.
class AdminConfig {
public:
    explicit AdminConfig(std::filesystem::path config);
    nlohmann::json load();
    nlohmann::json script(const std::string& command, const std::string& config_revision);
    nlohmann::json test_script(const std::string& command, const std::string& config_revision);
    nlohmann::json save(const nlohmann::json& request,
                        const std::function<void()>& after_first_write = {},
                        const std::function<void()>& after_script_write = {});
    static nlohmann::json generate(const std::string& user_id);
private:
    std::filesystem::path config_;
};
}

#include "tbot/notification_http.hpp"
#include "tbot/notification_protocol.hpp"

#include <arpa/inet.h>
#include <microhttpd.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace tbot {
namespace {

bool valid_text(std::string_view text) {
    if (text.empty()) return false;
    // Reject NUL, malformed/overlong UTF-8, surrogates and out-of-range scalars.
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first == 0) return false;
        if (first < 0x80) continue;
        unsigned value;
        unsigned count;
        unsigned minimum;
        if (first >= 0xc2 && first <= 0xdf) { value = first & 31; count = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 15; count = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 7; count = 3; minimum = 0x10000; }
        else return false;
        if (i + count > text.size()) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

bool credentials_equal(std::string_view actual, std::string_view expected) {
    std::size_t difference = actual.size() ^ expected.size();
    for (std::size_t i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(expected[i]) ^
            (i < actual.size() ? static_cast<unsigned char>(actual[i]) : 0);
    return difference == 0;
}

bool plain_utf8(const char* header) {
    if (!header) return false;
    std::string type(header);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto trim = [](std::string_view value) {
        const auto start = value.find_first_not_of(" \t");
        return start == std::string_view::npos ? std::string_view{} :
            value.substr(start, value.find_last_not_of(" \t") - start + 1);
    };
    const auto separator = type.find(';');
    if (trim(std::string_view(type).substr(0, separator)) != "text/plain") return false;
    if (separator == std::string::npos) return true;
    auto parameter = trim(std::string_view(type).substr(separator + 1));
    const auto equals = parameter.find('=');
    if (equals == std::string_view::npos || trim(parameter.substr(0, equals)) != "charset") return false;
    const auto charset = trim(parameter.substr(equals + 1));
    return charset == "utf-8" || charset == "\"utf-8\"";
}

MHD_Result respond(MHD_Connection* connection, unsigned status, const char* body) {
    auto* response = MHD_create_response_from_buffer(std::strlen(body), const_cast<char*>(body), MHD_RESPMEM_PERSISTENT);
    if (!response) return MHD_NO;
    MHD_add_response_header(response, "Content-Type", "application/json");
    MHD_add_response_header(response, "Cache-Control", "no-store");
    if (status == 401) MHD_add_response_header(response, "WWW-Authenticate", "Basic realm=\"tbot-notifications\", charset=\"UTF-8\"");
    if (status == 405) MHD_add_response_header(response, "Allow", "POST");
    if (status == 503) MHD_add_response_header(response, "Retry-After", "1");
    const auto result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}
} // namespace

struct NotificationHttpServer::Impl {
    struct Request { std::string body; bool too_large{false}; };
    NotificationHttpConfig config;
    std::function<bool(std::string)> enqueue;
    MHD_Daemon* daemon{nullptr};

    ~Impl() { if (daemon) MHD_stop_daemon(daemon); }

    static void completed(void*, MHD_Connection*, void** context, MHD_RequestTerminationCode) {
        delete static_cast<Request*>(*context);
        *context = nullptr;
    }

    static MHD_Result access(void* self, MHD_Connection* connection, const char* url,
                             const char* method, const char*, const char* data,
                             std::size_t* size, void** context) noexcept {
        try {
            return static_cast<Impl*>(self)->handle(connection, url, method, data, size, context);
        } catch (...) {
            return MHD_NO; // Never unwind through the C server or log request secrets.
        }
    }

    MHD_Result handle(MHD_Connection* connection, const char* url, const char* method,
                      const char* data, std::size_t* size, void** context) {
        if (!*context) {
            if (std::strcmp(url, "/v1/notifications") != 0)
                return respond(connection, 404, "{\"ok\":false,\"error\":\"not_found\"}");
            if (std::strcmp(method, "POST") != 0)
                return respond(connection, 405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
            char* password = nullptr;
            char* username = MHD_basic_auth_get_username_password(connection, &password);
            const bool user_ok = credentials_equal(username ? username : "", config.username);
            const bool pass_ok = credentials_equal(password ? password : "", config.password);
            MHD_free(username); MHD_free(password);
            if (!(user_ok & pass_ok))
                return respond(connection, 401, "{\"ok\":false,\"error\":\"unauthorized\"}");
            if (!plain_utf8(MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Type")) ||
                MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Encoding"))
                return respond(connection, 415, "{\"ok\":false,\"error\":\"unsupported_media_type\"}");
            if (const char* length = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Length")) {
                unsigned long long bytes{};
                const auto parsed = std::from_chars(length, length + std::strlen(length), bytes);
                if (parsed.ec == std::errc::result_out_of_range || bytes > maximum_notification_bytes)
                    return respond(connection, 413, "{\"ok\":false,\"error\":\"message_too_large\"}");
            }
            *context = new Request;
            return MHD_YES;
        }
        auto& request = *static_cast<Request*>(*context);
        if (*size) {
            if (*size > maximum_notification_bytes - request.body.size()) request.too_large = true;
            if (!request.too_large) request.body.append(data, *size);
            *size = 0;
            return MHD_YES;
        }
        if (request.too_large)
            return respond(connection, 413, "{\"ok\":false,\"error\":\"message_too_large\"}");
        if (!valid_text(request.body))
            return respond(connection, 400, "{\"ok\":false,\"error\":\"invalid_text\"}");
        if (!enqueue(std::move(request.body)))
            return respond(connection, 503, "{\"ok\":false,\"error\":\"queue_full\"}");
        return respond(connection, 202, "{\"ok\":true,\"status\":\"queued\"}");
    }
};

NotificationHttpServer::NotificationHttpServer(const NotificationHttpConfig& config,
                                               std::function<bool(std::string)> enqueue) {
    if (!config.enabled) return;
    impl_ = std::make_unique<Impl>();
    impl_->config = config;
    impl_->enqueue = std::move(enqueue);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(config.port));
    if (inet_pton(AF_INET, config.listen_address.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("Neplatná naslouchací HTTP adresa");
    impl_->daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, config.port, nullptr, nullptr,
        &Impl::access, impl_.get(),
        MHD_OPTION_SOCK_ADDR, reinterpret_cast<sockaddr*>(&address),
        MHD_OPTION_CONNECTION_LIMIT, static_cast<unsigned int>(8),
        MHD_OPTION_CONNECTION_TIMEOUT, static_cast<unsigned int>(5),
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, static_cast<std::size_t>(16384),
        MHD_OPTION_NOTIFY_COMPLETED, &Impl::completed, nullptr,
        MHD_OPTION_END);
    if (!impl_->daemon) throw std::runtime_error("Spuštění HTTP API selhalo; ověřte lokální IP adresu a obsazení portu");
}

NotificationHttpServer::~NotificationHttpServer() = default;
} // namespace tbot

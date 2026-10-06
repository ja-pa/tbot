#include "tbot/notification_http.hpp"
#include "tbot/notification_service.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

using namespace tbot;
using Json = nlohmann::json;

int free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address); assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    int port = ntohs(address.sin_port); close(fd); return port;
}

struct Reply { long status{}; std::string body; };
Reply request(int port, const std::string& message, const char* credentials = "device:private-password",
              const char* type = "text/plain; charset=utf-8", const char* path = "/v1/notifications",
              const char* method = "POST", bool chunked = false) {
    CURL* curl = curl_easy_init(); assert(curl);
    Reply reply;
    const auto url = "http://127.0.0.1:" + std::to_string(port) + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, message.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(message.size()));
    if (credentials) curl_easy_setopt(curl, CURLOPT_USERPWD, credentials);
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, (std::string("Content-Type: ") + type).c_str());
    headers = curl_slist_append(headers, "Expect:");
    if (chunked) headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* data, std::size_t size, std::size_t count, void* ptr) {
        static_cast<std::string*>(ptr)->append(data, size * count); return size * count;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply.body);
    const auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) std::cerr << "HTTP test request failed: " << message.size() << " bytes, " << path << ": " << curl_easy_strerror(result) << '\n';
    assert(result == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.status);
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    return reply;
}

template<class F> void rejects(F f) { bool rejected = false; try { f(); } catch (...) { rejected = true; } assert(rejected); }

int stalled_upload(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const char partial[] = "POST /v1/notifications HTTP/1.1\r\nHost: localhost\r\n"
        "Authorization: Basic ZGV2aWNlOnByaXZhdGUtcGFzc3dvcmQ=\r\n"
        "Content-Type: text/plain\r\nContent-Length: 10\r\n\r\nx";
    assert(send(fd, partial, sizeof(partial)-1, 0) == sizeof(partial)-1);
    return fd;
}

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Json document = {{"telegram", {{"token", "x"}, {"allowed_user_ids", {1}}}},
                     {"notifications", {{"chat_ids", {1}}}}};
    const Json commands = {{"commands", Json::array()}};
    auto parse = [&](const Json& http) {
        auto copy = document; copy["notifications"]["http"] = http;
        return TbotConfig::from_json("/tmp/http-config.json", copy, commands);
    };
    assert(!TbotConfig::from_json("/tmp/http-config.json", document, commands).notifications().http.enabled);
    for (const auto& bad : {Json(nullptr), Json::array(), Json("bad")}) rejects([&] { parse(bad); });
    for (const auto& bad : {Json(0), Json(65536), Json(8088.5), Json("8088"), Json(UINT64_MAX)})
        rejects([&] { parse({{"port", bad}}); });
    for (const char* bad : {"0.0.0.0", "0.1.2.3", "224.0.0.1", "255.255.255.255", "::1", "localhost", "1.2.3.999"})
        rejects([&] { parse({{"listen_address", bad}}); });
    rejects([&] { parse({{"enabled", true}}); });
    rejects([&] { parse({{"enabled", "false"}}); });
    rejects([&] { parse({{"username", "a:b"}}); });
    rejects([&] { parse({{"password", std::string(257, 'a')}}); });
    rejects([&] { parse({{"password", std::string("a\0b", 3)}}); });

    NotificationHttpConfig config{true, "127.0.0.1", free_port(), "device", "private-password"};
    NotificationQueue queue;
    std::mutex mutex;
    auto enqueue = [&](std::string message) { std::lock_guard lock(mutex); return queue.try_push(std::move(message)); };
    auto pop = [&] { std::lock_guard lock(mutex); std::string message; assert(queue.try_pop(message)); return message; };
    {
        NotificationHttpServer server(config, enqueue);
        rejects([&] { NotificationHttpServer duplicate(config, enqueue); });
        auto disabled_config = config; disabled_config.enabled = false;
        NotificationHttpServer disabled(disabled_config, enqueue); // Does not bind an already occupied port.
        const std::string message = "Backup complete\nSecond line ✓";
        auto reply = request(config.port, message);
        assert(reply.status == 202 && Json::parse(reply.body)["status"] == "queued");
        assert(pop() == message);
        assert(request(config.port, "x", nullptr).status == 401);
        assert(request(config.port, "x", "device:wrong").status == 401);
        assert(request(config.port, "x", "wrong:private-password").status == 401);
        assert(request(config.port, "x", "device:private-password", "application/json").status == 415);
        assert(request(config.port, "x", "device:private-password", "text/plain;charset=latin1").status == 415);
        assert(request(config.port, "x", "device:private-password", "text/plain", "/missing").status == 404);
        assert(request(config.port, "x", "device:private-password", "text/plain", "/v1/notifications", "GET").status == 405);
        assert(request(config.port, "").status == 400);
        for (const auto& bad : {std::string("x\0y", 3), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("\xc4")})
            assert(request(config.port, bad).status == 400);
        assert(request(config.port, std::string(3500, 'a')).status == 202); assert(pop().size() == 3500);
        assert(request(config.port, std::string(3501, 'a')).status == 413);
        assert(request(config.port, std::string(100000, 'a')).status == 413);
        assert(request(config.port, std::string(3501, 'a'), "device:private-password", "text/plain",
                       "/v1/notifications", "POST", true).status == 413);
        assert(request(config.port, message, "device:private-password", "text/plain",
                       "/v1/notifications", "POST", true).status == 202); assert(pop() == message);
        std::string multibyte;
        // Two-byte UTF-8 characters exercise the byte limit, not a character limit.
        for (int i = 0; i < 1750; ++i) multibyte += "á";
        assert(request(config.port, multibyte).status == 202); assert(pop() == multibyte);
        assert(request(config.port, "x", "device:private-password", "Text/Plain ; charset = \"UTF-8\"").status == 202); assert(pop() == "x");
        for (int i = 0; i < 10; ++i) assert(request(config.port, std::to_string(i)).status == 202);
        assert(request(config.port, "overflow").status == 503);
        assert(pop() == "0"); // Rejection must not evict the oldest message.
        for (int i = 1; i < 10; ++i) assert(pop() == std::to_string(i));
        assert(request(config.port, "recovered").status == 202); assert(pop() == "recovered");
    }
    {
        NotificationHttpServer restarted(config, enqueue);
        assert(request(config.port, "restart").status == 202); assert(pop() == "restart");
        int idle = stalled_upload(config.port);
        timeval timeout{8, 0};
        assert(setsockopt(idle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        char byte;
        const auto received = recv(idle, &byte, 1, 0);
        assert(received == 0 || (received < 0 && errno == ECONNRESET));
        close(idle);
        assert(request(config.port, "after-timeout").status == 202); assert(pop() == "after-timeout");
    }
    // A stalled client must not hold up server destruction.
    int stalled;
    const auto start = std::chrono::steady_clock::now();
    {
        NotificationHttpServer server(config, enqueue);
        stalled = stalled_upload(config.port);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2)); close(stalled);
    // Exercise actual shared service and Unix receiver concurrently, without Telegram traffic.
    const auto socket_path = "/tmp/tbot-http-test-" + std::to_string(getpid()) + ".sock";
    {
        NotificationService service("unused", {}, socket_path);
        NotificationHttpServer server(config, [&](std::string message) { return service.try_enqueue(std::move(message)); });
        std::thread local([&] {
            int fd = socket(AF_UNIX, SOCK_DGRAM, 0); assert(fd >= 0);
            sockaddr_un address{}; address.sun_family = AF_UNIX;
            std::strcpy(address.sun_path, socket_path.c_str());
            for (int i = 0; i < 10; ++i)
                assert(sendto(fd, "local", 5, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 5);
            close(fd);
        });
        for (int i = 0; i < 10; ++i) {
            auto reply = request(config.port, "remote"); assert(reply.status == 202 || reply.status == 503);
        }
        local.join();
    }
    assert(access(socket_path.c_str(), F_OK) != 0);
    curl_global_cleanup();
}

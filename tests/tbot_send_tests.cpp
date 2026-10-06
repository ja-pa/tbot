#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>

int main(int argc, char* argv[]) {
    assert(argc == 2);

    const auto socket_path = std::filesystem::temp_directory_path() /
                             ("tbot-send-test-" + std::to_string(getpid()) + ".sock");
    const std::string socket_text = socket_path.string();

    const int server = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(server >= 0);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    assert(socket_text.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, socket_text.c_str(), socket_text.size() + 1);
    assert(bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(argv[1], argv[1], "--socket", socket_text.c_str(), "testovací zpráva", nullptr);
        _exit(127);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

    char buffer[128]{};
    const ssize_t received = recv(server, buffer, sizeof(buffer), 0);
    assert(received > 0);
    assert(std::string(buffer, static_cast<std::size_t>(received)) == "testovací zpráva");

    close(server);
    unlink(socket_path.c_str());
    return 0;
}

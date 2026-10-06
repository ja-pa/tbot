# tbot — Telegram commands and notifications for Linux

A small C++20 Telegram bot with an allowed-user list, command buttons, TOTP
two-factor authentication and custom shell scripts. Send notifications from
local scripts through a Unix socket, or from your LAN/VPN through an optional
HTTP endpoint.

OpenWrt packages, LuCI and router scripts live in
[tbot-wrt](https://github.com/ja-pa/tbot-wrt).

## Build

On Ubuntu or Ubuntu under WSL, from the repository root:

```sh
sudo apt install build-essential cmake ninja-build git libcurl4-openssl-dev libmicrohttpd-dev pkg-config
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires Linux, CMake 3.22+, a C++20 compiler and libmicrohttpd 0.9.71+.
See the [development guide](docs/development.md) for dependencies and build options.

## Quick start

Create a bot with [@BotFather](https://t.me/BotFather), then copy the examples:

```sh
cp config/examples/config.json config.json
cp config/examples/commands.json commands.json
chmod 600 config.json
```

Set `telegram.token`, `telegram.allowed_user_ids` and `notifications.chat_ids`
in `config.json`. The [user guide](docs/user-guide.md#telegram-setup) explains
how to find these IDs.

```sh
./build/tbot -c config.json
# In another terminal, while the bot is running:
./build/tbot-send "Backup complete"
```

Send `/start` or `/help` to the bot. The example command list is empty; add
your own scripts when ready. Press Ctrl+C to stop.

## Documentation

| Guide | Contents |
| --- | --- |
| [User guide](docs/user-guide.md) | Telegram setup, configuration, commands, 2FA, notifications and troubleshooting |
| [Development guide](docs/development.md) | Build options, architecture, protocols, administration backend and tests |

Built-in text is English. User-defined descriptions, button labels and messages
can use other languages. Existing configuration, socket paths and paired 2FA
secrets remain compatible.

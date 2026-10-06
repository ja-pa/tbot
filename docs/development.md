# tbot development guide

[README](../README.md) · [User guide](user-guide.md)

## Build and test

On Ubuntu / WSL, from the repository root:

```sh
sudo apt install build-essential cmake ninja-build git libcurl4-openssl-dev libmicrohttpd-dev pkg-config
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires Linux, CMake 3.22+, C++20 and libmicrohttpd 0.9.71+. `./rebuild.sh`
performs a clean build and runs tests, preserving local `config.json`.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `TBOT_USE_SYSTEM_JSON` | `OFF` | Use installed nlohmann/json headers instead of fetching v3.12.0 |
| `TBOT_BUILD_OPENWRT_ADMIN` | `OFF` | Build the administration library and executable |
| `BUILD_TESTING` | `ON` | Build and register tests; disable for cross-compilation |

For system JSON headers, install `nlohmann-json3-dev` and configure with
`-DTBOT_USE_SYSTEM_JSON=ON`. Tiny2FA and qrcodegen are vendored in `third_party`
with their licenses; do not rewrite upstream sources as part of text changes.

## Architecture

The project can be embedded with `add_subdirectory`. `tbot_core` exposes
`tbot/*.hpp` in namespace `tbot`. Executables are `tbot` and `tbot-send`.

| Component | Responsibility |
| --- | --- |
| Application and command registry | Polling loop, authorization, dispatch, menus and built-in commands |
| Configuration | JSON parsing, paths and shared validation |
| Telegram client | libcurl requests and Telegram API responses |
| Script runner | Execution timeout and bounded output |
| TwoFactor and QR code | Tiny2FA TOTP sessions and local qrcodegen rendering |
| Notification service | Unix socket, bounded queue and Telegram delivery |
| Notification HTTP server | Optional libmicrohttpd listener feeding the shared queue |
| OpenWrt administration | Configuration transactions, script editing/testing and service control |

Custom C++ commands set `Command.requires_2fa` after `handler` when needed;
it defaults to `false`. `auth` and `lock` remain reserved. See the
[user guide](user-guide.md#custom-commands) for configuration-backed commands.

Keep comments, diagnostics, built-in labels and documentation in English.
User data remains Unicode. Preserve CLI options, configuration keys, RPC names,
HTTP error codes and exit statuses when changing human-readable text.

## Notification contracts

The local client sends a Unix datagram to the configured socket, default
`/tmp/tbot-wrt.sock`. The message limit is 3500 bytes; successful submission does
not acknowledge Telegram delivery. The shared in-memory queue holds ten messages
and sends at intervals of at least one second. Local overflow drops the oldest
message; HTTP overflow rejects the incoming message. There is no persistent
queue or automatic delivery retry.

### HTTP contract

`POST /v1/notifications` requires HTTP Basic authentication and `Content-Type:
text/plain`, optionally with `charset=utf-8`. Plain text without a charset also
means UTF-8. The body must contain 1–3500 bytes of valid UTF-8 without NUL. The
server preserves content and line breaks. Attachments, content encoding and
alternative media types are unsupported. Recipients are configured server-side.

| HTTP status | Response / meaning |
| --- | --- |
| 202 | `{"ok":true,"status":"queued"}` |
| 400 | `invalid_text`: empty body, NUL or invalid UTF-8 |
| 401 | `unauthorized`: missing or incorrect credentials |
| 404 | `not_found`: unknown path |
| 405 | `method_not_allowed`: requires POST |
| 413 | `message_too_large` |
| 415 | `unsupported_media_type` |
| 503 | `queue_full`, with `Retry-After: 1` |

Application errors use `{"ok":false,"error":"error_code"}`. The HTTP library
may reject malformed framing itself. Limits are eight connections, five seconds
of inactivity and 16 KiB of memory per connection. Acceptance is not a delivery
guarantee, and retrying after a network failure can produce duplicates.

## Optional OpenWrt backend

Sources are in `src/openwrt`, public headers in `include/tbot/openwrt`, and tests
in `tests/openwrt`. Enable the backend and its configuration tests with:

```sh
cmake -S . -B build/admin -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTBOT_BUILD_OPENWRT_ADMIN=ON
cmake --build build/admin
ctest --test-dir build/admin --output-on-failure
```

This creates `tbot_wrt_admin` and `tbot-wrt-admin`, registers
`admin-configuration` when testing is enabled, and installs the executable in
`libexec`. Configuration tests run on Linux; live service control requires
OpenWrt ubus/procd. The frontend, RPC shell adapter and init script belong to
[tbot-wrt](https://github.com/ja-pa/tbot-wrt). Its
[development guide](https://github.com/ja-pa/tbot-wrt/blob/main/docs/development.md)
documents the RPC interface, transaction format, packaging and browser tests.

## Verification

CTest covers configuration and command dispatch, disabled callbacks, TOTP,
QR rendering, local notifications, HTTP validation and CLI behavior. Backend
tests additionally cover unknown JSON fields, int64 IDs, secret masking,
validation, revision conflicts and recovery after write errors or process death.

When changing text, update existing assertions without weakening behavior checks.
Keep Unicode and byte-limit fixtures, and check `tbot --help`, `tbot-send --help`,
Telegram menus and error paths. No live Telegram credentials are needed for the
local tests. For integration changes, also run the parent repository checks.

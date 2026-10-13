# tbot user guide

[README](../README.md) · [Development guide](development.md)

## Install and run

Build the Linux binaries using the [README](../README.md#build). For an OpenWrt
router, use the [tbot-wrt installation guide](https://github.com/ja-pa/tbot-wrt/blob/main/docs/user-guide.md).

From the repository root:

```sh
cp config/examples/config.json config.json
cp config/examples/commands.json commands.json
chmod 600 config.json
```

Complete the Telegram settings below, then start with `./build/tbot -c config.json`.
Without `-c`, the program shows help. Stop it with Ctrl+C or SIGTERM.

## Telegram setup

1. Open the official [@BotFather](https://t.me/BotFather) account and send `/newbot`.
2. Choose a display name and a unique username ending in `bot`.
3. Save the returned token privately. Treat it as a password and never commit it.
4. Open your new bot and send `/start`.
5. With the bot daemon stopped, run this in Bash to inspect pending updates:

   ```bash
   read -rsp "Telegram bot token: " TELEGRAM_BOT_TOKEN; echo
   curl -s "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/getUpdates" \
     | python3 -m json.tool
   unset TELEGRAM_BOT_TOKEN
   ```

Use `result[].message.from.id` for `telegram.allowed_user_ids`, and
`result[].message.chat.id` for `notifications.chat_ids`. Usernames are not used
for authorization. Group chat IDs can be negative.

## Configuration

Start with [config/examples/config.json](../config/examples/config.json).

| Setting | Purpose |
| --- | --- |
| `telegram.token` | BotFather token |
| `telegram.allowed_user_ids` | Nonempty list of positive Telegram user IDs allowed to run commands |
| `telegram.poll_timeout_seconds` | Long-polling timeout, 1–50 seconds; default 30 |
| `telegram.two_factor_users` | Optional list of user IDs and individual TOTP secrets |
| `notifications.chat_ids` | Required, nonempty list of notification recipients; IDs must not be zero |
| `notifications.socket_path` | Absolute Unix socket path; default `/tmp/tbot-wrt.sock` |
| `notifications.http` | Optional HTTP listener, disabled by default |
| `commands_file` | Command configuration path; default `commands.json` |

`commands_file` is relative to the main configuration file. Script paths are
relative to the command configuration file. Restart the bot after configuration
changes. Edits to an existing script take effect on its next invocation.

The example command list is empty. Built-in commands are `/start`, `/help`,
`/version`, `/auth` and `/lock`; they need no router utilities.

## Custom commands

Add entries to `commands.json`:

```json
{
  "commands": [{
    "name": "hello",
    "description": "Local greeting",
    "button": {"text": "Say hello", "row": 0},
    "script": "scripts/hello.sh",
    "enabled": true,
    "requires_2fa": false
  }]
}
```

Create an executable script relative to that file:

```sh
mkdir -p scripts
printf '#!/bin/sh\nprintf "Hello\\n"\n' > scripts/hello.sh
chmod +x scripts/hello.sh
```

Names use lowercase letters, digits and underscores. Descriptions must not be
empty. Omit `button` for a text-only command; if present, its text must be
nonempty and its row an integer from 0 to 100. Buttons sharing a row appear
side by side; empty rows are skipped. Built-in Help uses row 2.

Scripts run without arguments under the bot process account, using the system
`timeout` command with a 10-second limit and about 3800 bytes of output. Paths
cannot contain spaces or shell metacharacters. Scripts must exist and be
executable even for disabled commands. `enabled: false` hides a command and
blocks text invocations and old callbacks. Restart after changing the command list.

## Two-factor authentication

### Set up a user

Generate a secret in a local terminal or over SSH:

```sh
tbot --generate-2fa-secret 123456789
# Short form, with portable ASCII QR output:
tbot -g 123456789 -a
```

Use `./build/tbot` if it is not installed on your PATH. This standalone command
cannot be combined with `--config`. It prints a JSON object, an `otpauth://`
URI and a QR code without starting the bot or changing configuration. Interactive
terminals use compact Unicode blocks in black and white; `--qr-ascii` / `-a`
forces two-character ASCII output. Randomness comes from Linux `getrandom()`;
failure stops generation.

Scan the QR code with your authenticator, or enter the secret manually as a
time-based token. Label accounts so you can distinguish them. The URI contains
the same secret: do not send it through Telegram or use a public QR generator.

Copy the generated object into `telegram.two_factor_users`:

```json
"two_factor_users": [
  {"user_id": 123456789, "secret": "REPLACE_WITH_GENERATED_32_CHARACTER_SECRET"}
]
```

Replace the placeholder with exactly 32 Base32 characters (`A-Z` and `2-7`, no
spaces). Each user must also be in `allowed_user_ids`. Duplicate IDs, shared
secrets and invalid secrets prevent startup. An omitted or empty list leaves
ordinary commands available but prevents anyone from authenticating.

Keep `config.json` private (`chmod 600 config.json`) and restart after changes.
If an authenticator is lost, replace its secret locally and pair the new one.
CLI options have long and short forms: `--generate-2fa-secret` / `-g`,
`--qr-ascii` / `-a`, `--config` / `-c`, and `--help` / `-h`.

### Protect a command

Set `"requires_2fa": true` on the command entry and restart. The default is
`false`, so existing commands are not automatically protected. `auth` and `lock`
are reserved names.

### Authenticate and lock

- Send `/auth 123456` in a private chat using the current authenticator code.
- Successful authentication unlocks all protected commands for that user for
  300 seconds and shows a new menu. Running commands does not extend the session;
  authenticating with a new valid code starts another five minutes.
- `/lock` ends access immediately. Restarting the bot ends all sessions.
- Authentication and protected commands work only in private chats. Groups do
  not show or execute protected commands, even after private authentication.
- Without authentication, protected commands behave as unknown commands,
  including button text and callbacks from old menus.
- Five failed attempts block authentication for that user for five minutes.
  `/lock` does not reset this limit; each user has an independent limit.

TOTP uses SHA-1, six digits and 30-second steps. The bot accepts the current
step and adjacent steps (±30 seconds), but never an already accepted or older
step. Keep the phone and host clocks accurate. Session and lockout durations use
a monotonic clock. Replay protection and attempt limits are in memory and reset
on restart, so a still-valid code may work again after a restart.

The bot removes protected buttons from its latest unlocked menu on expiry,
locking and orderly shutdown. Updates may be delayed by scripts or network
outages; a crash may leave old buttons visible. Authorization is always checked
before execution. Previously sent answers remain in chat history.

## Local notifications

`tbot-send` lets cron jobs and scripts pass a message to the running daemon. It
does not read the Telegram token, access the network or wait for delivery.

```sh
tbot-send "Router restarted"
printf '%s\n' "WAN connection lost" | tbot-send
tbot-send --socket /tmp/tbot-test.sock "Test message"
```

A custom socket path must match the daemon configuration. Messages are limited
to 3500 bytes. The in-memory queue holds ten messages; a local sender filling it
discards the oldest message and logs the event. Delivery is limited to one
message per second. Failed Telegram deliveries are not retried automatically.

## HTTP notifications from LAN or VPN

Add or enable this section inside `notifications`:

```json
"http": {
  "enabled": true,
  "listen_address": "192.168.1.1",
  "port": 8088,
  "username": "notifier",
  "password": "REPLACE_WITH_YOUR_OWN_PASSWORD"
}
```

Use a specific local unicast IPv4 address, not `0.0.0.0`. `127.0.0.1` allows
local access only. The default port is 8088. When enabled, both credentials must
be nonempty, at most 256 bytes each, without NUL, CR or LF; usernames cannot
contain a colon. These credentials are separate from Telegram and OS accounts.
Restart after saving. An unassigned address or occupied port prevents startup.
Without this section, or with `enabled: false`, no TCP port is opened.

HTTP Basic sends credentials and messages unencrypted. Use a trusted LAN or VPN,
keep the configuration at mode `0600`, and do not expose the port to the WAN.
The bot does not change firewall rules.

```sh
# curl prompts for the password; do not put it in the URL.
curl --fail-with-body --user notifier \
  -H 'Content-Type: text/plain; charset=utf-8' \
  --data-binary 'Server backup complete' \
  http://192.168.1.1:8088/v1/notifications

# Preserve a multiline report:
curl --fail-with-body --user notifier \
  -H 'Content-Type: text/plain; charset=utf-8' \
  --data-binary @report.txt \
  http://192.168.1.1:8088/v1/notifications
```

From the repository root, the [Bash example](../examples/tbot-send-http.sh) offers
the same operation with one message argument:

```bash
bash examples/tbot-send-http.sh "Backup complete"
TBOT_URL=http://192.168.1.1:8088 TBOT_USER=notifier \
  bash examples/tbot-send-http.sh "Message from a computer"
```

It requires Bash and curl with `--fail-with-body`. `--help` shows usage. curl
prompts for the HTTP API password, not the router SSH password. HTTP errors print
the response and return a nonzero exit code.

The endpoint accepts 1–3500 bytes of plain UTF-8 text without NUL, preserving
line breaks. Recipients always come from `notifications.chat_ids`. Attachments
and compression are unsupported. `202` means queued, not delivered; `503` means
the queue is full. See the [HTTP contract](development.md#http-contract) for all
response codes.

HTTP and the local socket share the same queue. Messages can be lost on restart,
Telegram failure or overflow caused by local senders. Retrying after a network
error may duplicate a message. There is no persistent queue or delivery receipt.
Success logs report the number of chats accepted by the Telegram API without
logging message contents or credentials.

## Troubleshooting and compatibility

| Symptom | Check |
| --- | --- |
| Help appears instead of startup | Supply `-c config.json` |
| Startup fails | Read the error; check JSON, token, nonempty ID lists, script permissions and 2FA secrets |
| Command is unknown | Check allowed user ID, enabled state, private chat and 2FA session |
| 2FA fails | Check both clocks, secret, reused codes and the five-minute attempt lockout |
| Local notification fails | Check that the daemon is running and socket paths match |
| HTTP startup fails | Check that the configured IP belongs to the host and the port is free |
| HTTP returns 401 | Use the configured HTTP credentials, not a Telegram token or SSH password |
| HTTP returns 202 but no message arrives | Check destination chat IDs and daemon logs; acceptance does not guarantee delivery |

The default socket `/tmp/tbot-wrt.sock` and TOTP issuer `tbot-wrt` remain unchanged
for existing installations. Configuration format and paired secrets are compatible.
English built-in text does not rewrite user-defined labels, scripts or messages.

## Menu, authentication and custom help

The bottom menu row contains **Auth** (or **Lock** while authenticated) and
**Help**. Authentication controls appear only in private chats. Tap **Auth** or
send `/auth`, then paste the six-digit authenticator code as a separate message
within two minutes. A failed attempt can be retried within that window. Five
failed attempts block verification for five minutes. `/auth 123456` also works.
Successful authentication lasts five minutes; **Lock** or `/lock` ends it and
cancels pending code entry. After access expires, the menu returns to **Auth**.

Set `telegram.help_text` in `config.json` to customize the entire Help response
without recompiling. Use `\n` for line breaks in the JSON string and restart the
bot after editing. If omitted, the bot uses its built-in English help. The text
is maintained manually: add descriptions of your configured script commands as
needed. The bot does not append or generate a command list.

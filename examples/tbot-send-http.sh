#!/usr/bin/env bash
set -euo pipefail

# Match the router's notifications.http settings. curl prompts for the API password.
TBOT_URL="${TBOT_URL:-http://192.168.1.1:8088}"
TBOT_USER="${TBOT_USER:-notifier}"

usage() {
    printf 'Použití: bash %s "Zpráva do Telegramu"\n' "${0##*/}"
    printf 'Nastavení: TBOT_URL (adresa a port routeru), TBOT_USER (HTTP uživatel).\n'
    printf 'Heslo HTTP API si vyžádá curl. Úspěch znamená přijetí do fronty.\n'
}

if [[ "${1:-}" == '--help' || "${1:-}" == '-h' ]]; then
    usage
    exit 0
fi
if (( $# != 1 )) || [[ -z "$1" ]]; then
    usage >&2
    exit 2
fi

# stdin keeps leading @ characters literal instead of treating the message as a file.
printf '%s' "$1" | curl --silent --show-error --fail-with-body \
    --connect-timeout 5 --max-time 15 \
    --user "$TBOT_USER" \
    --header 'Content-Type: text/plain; charset=utf-8' \
    --data-binary @- \
    "${TBOT_URL%/}/v1/notifications"
printf '\n'

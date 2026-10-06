# tbot

Malý Telegram bot pro Linux v C++20. Obsahuje long polling přes libcurl,
allowlist uživatelů, menu příkazů, TOTP 2FA, spouštění nakonfigurovaných skriptů
a lokální notifikace přes Unix socket. Volitelně obsahuje také OpenWrt administrační backend. OpenWrt balíčky, LuCI a routerové skripty
jsou v [tbot-wrt](https://github.com/ja-pa/tbot-wrt).

## Sestavení a testy

V Ubuntu / WSL:

```bash
sudo apt install build-essential cmake ninja-build git libcurl4-openssl-dev libmicrohttpd-dev pkg-config
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Další čistý build s testy spustí `./rebuild.sh`. CMake standardně stáhne
`nlohmann/json` v3.12.0. S `-DTBOT_USE_SYSTEM_JSON=ON` použije již nainstalované
hlavičky (v Ubuntu balíček `nlohmann-json3-dev`). Tiny2FA a qrcodegen jsou
přibalené v `third_party` včetně licencí.

Projekt lze vložit pomocí `add_subdirectory`; knihovna `tbot_core` poskytuje
hlavičky `tbot/*.hpp` a namespace `tbot`. Programy jsou `tbot` a `tbot-send`.
Při cross-kompilaci lze testy vypnout přes `-DBUILD_TESTING=OFF`.

## HTTP notifikace z LAN / VPN

Volitelná sekce `notifications.http` zpřístupní odesílání zpráv z jiných
zařízení. Bez sekce nebo s `enabled: false` daemon žádný TCP port neotevírá.
Příklad sekce uvnitř `notifications`:

```json
"http": {
  "enabled": true,
  "listen_address": "192.168.1.1",
  "port": 8088,
  "username": "notifier",
  "password": "REPLACE_WITH_YOUR_OWN_PASSWORD"
}
```

Adresa musí být konkrétní lokální unicast IPv4 routeru (nikoli `0.0.0.0`);
`127.0.0.1` umožňuje jen místní přístup. Výchozí port je 8088. Při zapnutí
musí být jméno i heslo neprázdné, nejvýše 256 bajtů, bez NUL/CR/LF;
jméno nesmí obsahovat dvojtečku. Jde o samostatné přihlášení, ne Telegram
token ani účet operačního systému. Uložení konfigurace vyžaduje restart.
Nepřiřazená adresa nebo obsazený port způsobí chybu startu služby.

HTTP Basic přenáší přihlášení a zprávu v LAN nešifrovaně. Používejte pouze
důvěryhodnou LAN nebo VPN. Firewall se automaticky nemění; port nevystavujte
na WAN. Heslo je uložené v konfiguraci, kterou chraňte právy `0600`.

```sh
# curl si vyžádá heslo; nepřidávejte ho do URL.
curl --fail-with-body --user notifier \
  -H 'Content-Type: text/plain; charset=utf-8' \
  --data-binary 'Záloha serveru dokončena' \
  http://192.168.1.1:8088/v1/notifications

# Víceřádková zpráva ze souboru:
curl --fail-with-body --user notifier \
  -H 'Content-Type: text/plain; charset=utf-8' \
  --data-binary @report.txt \
  http://192.168.1.1:8088/v1/notifications
```

Hotový Bash příklad je v [examples/tbot-send-http.sh](examples/tbot-send-http.sh).
Z kořene repozitáře `tbot` ho spustíte s jedním argumentem:

```bash
bash examples/tbot-send-http.sh "Záloha dokončena"

# Vlastní adresa a uživatelské jméno:
TBOT_URL=http://192.168.1.1:8088 TBOT_USER=notifier \
  bash examples/tbot-send-http.sh "Zpráva z počítače"
```

Heslo HTTP API si vyžádá `curl`; nejde o SSH heslo routeru. Skript vyžaduje
Bash a curl s podporou `--fail-with-body`. Při HTTP chybě skončí nenulovým
kódem a vypíše odpověď serveru. Volba `--help` zobrazí použití.

API přijímá pouze `POST /v1/notifications` s prostým UTF-8 textem, 1–3500
bajtů, bez NUL. `text/plain` bez parametru znamená také UTF-8. Obsah se
předává beze změn, včetně konců řádků. Přílohy, komprese a jiné typy obsahu
nejsou podporovány. Příjemce určuje výhradně `notifications.chat_ids`.

| HTTP | Význam |
| --- | --- |
| 202 | `{"ok":true,"status":"queued"}` — přijato do fronty |
| 400 | Prázdný text, NUL nebo neplatné UTF-8 |
| 401 | Chybějící nebo nesprávné přihlášení |
| 404 / 405 | Neznámá cesta / nepovolená metoda |
| 413 / 415 | Příliš dlouhá zpráva / nepodporovaný obsah |
| 503 | Plná fronta; zpráva nebyla přijata (`Retry-After: 1`) |

Chyby aplikace vracejí JSON `{"ok":false,"error":"kod_chyby"}`.
Vadné HTTP rámování může odmítnout samotná HTTP knihovna. Server omezuje
počet spojení na 8, nečinnost na 5 sekund a paměť spojení na 16 KiB.

HTTP a lokální socket sdílejí frontu 10 zpráv a interval odesílání alespoň
jednu sekundu mezi zprávami. HTTP při plné frontě odmítne novou zprávu;
lokální socket nadále zahazuje nejstarší. Přijetí proto nezaručuje doručení:
zpráva může zaniknout při restartu, chybě Telegramu nebo zaplnění lokálním
odesílatelem. Automatické opakování HTTP požadavku po síťové chybě může
vytvořit duplicitu. Trvalá fronta ani potvrzení doručení nejsou součástí API.
`tbot-send` zůstává lokálním klientem.
Log úspěšného odeslání uvádí počet chatů, kterým Telegram API potvrdilo
přijetí; neobsahuje text zprávy ani přihlašovací údaje.

## Volitelná OpenWrt administrace

C++ backend `tbot-wrt-admin` je v `src/openwrt`, hlavička v
`include/tbot/openwrt` a testy v `tests/openwrt`. Standardně se nesestavuje.
Zapnutí vytvoří také knihovnu `tbot_wrt_admin` a při `BUILD_TESTING=ON`
zaregistruje test `admin-configuration`:

```bash
cmake -S . -B build/admin -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTBOT_BUILD_OPENWRT_ADMIN=ON
cmake --build build/admin
ctest --test-dir build/admin --output-on-failure
```

Instalace zapnutého backendu přidá `libexec/tbot-wrt-admin`. Jeho konfiguraci
lze testovat na Linuxu; řízení služby za běhu vyžaduje OpenWrt ubus/procd.
LuCI frontend, RPC shellový adaptér a init skript dodává `tbot-wrt`.

## První spuštění

```bash
cp config/examples/config.json config.json
cp config/examples/commands.json commands.json
chmod 600 config.json
# Doplň token, allowed_user_ids a notifications.chat_ids.
./build/tbot -c config.json
```

Příklad `commands.json` je prázdný: fungují vestavěné `/start`, `/help`,
`/version`, `/auth` a `/lock`, bez routerových závislostí. Bez `-c` program
vypíše nápovědu. Ukončení: Ctrl+C nebo SIGTERM.

`notifications` je povinná sekce a potřebuje neprázdné `chat_ids`.
`allowed_user_ids` určuje, kdo smí zadávat příkazy; `chat_ids` určuje příjemce
lokálních notifikací. Cesta `commands_file` je relativní ke konfiguraci bota;
cesty skriptů jsou relativní ke konfiguraci příkazů.

## Vlastní příkazy

```json
{
  "commands": [{
    "name": "hello",
    "description": "Lokální pozdrav",
    "button": {"text": "Pozdrav", "row": 0},
    "script": "scripts/hello.sh",
    "enabled": true,
    "requires_2fa": false
  }]
}
```

Vytvoř vlastní spustitelný `scripts/hello.sh`, například s `printf 'Ahoj\n'`.
Skripty se spouštějí bez argumentů přes systémový `timeout`, s limitem 10 sekund
a přibližně 3800 bajtů výstupu. Cesta nesmí obsahovat mezery ani shellové
speciální znaky. Skript musí existovat a být spustitelný i pro vypnutý příkaz.
`enabled: false` skryje příkaz a zablokuje text i staré callbacky.
Po změně konfigurace restartuj bota. Skripty běží s oprávněním procesu bota.

## Ochrana příkazů pomocí 2FA

Bot obsahuje C knihovnu Tiny2FA obalenou třídou `TwoFactor`. Používá TOTP
(SHA-1, 6 číslic, interval 30 sekund), kompatibilní s autentizačními aplikacemi.
Zdroj knihovny je připnutý a uložený v `third_party/tiny2fa`, takže sestavení nepotřebuje další balíček ani stahování této knihovny.

### Založení uživatele

Pro každého uživatele spusť přes lokální terminál nebo SSH:

```sh
tbot --generate-2fa-secret 123456789
# zkráceně:
tbot -g 123456789
```

Samostatný příkaz bez `--config` přijme Telegram `user_id` a vypíše hotový JSON
objekt s `user_id` a novým 32znakovým Base32 klíčem, `otpauth://` URI a QR kód,
který lze rovnou naskenovat z SSH terminálu. JSON objekt vlož do pole
`telegram.two_factor_users`.
Nespouští bota a nemění konfiguraci. Na interaktivním terminálu používá kompaktní
Unicode bloky s pevně nastavenou černou a bílou barvou; volba `--qr-ascii` nebo
`-a`
vynutí přenosnější dvouznakový ASCII výstup. Náhoda pochází z Linux
`getrandom()`; při jeho selhání generování skončí chybou. Klíč ani URI neposílej
přes Telegram a nepoužívej veřejné webové generátory QR kódů. Klíč přidej ručně
do autentizační aplikace jako časový token; každý účet si v aplikaci pojmenuj
tak, aby šel rozlišit. URI obsahuje tentýž tajný klíč.

Všechny CLI volby mají dlouhý i krátký tvar:

- `--generate-2fa-secret` / `-g`;
- `--qr-ascii` / `-a`;
- `--config` / `-c`;
- `--help` / `-h`.

Do sekce `telegram` v soukromém `config.json` doplň mapování:

```json
"two_factor_users": [
  {"user_id": 123456789, "secret": "SEM_VLOZ_VYGENEROVANY_32ZNAKOVY_KLIC"}
]
```

Ukázkový zástupný text nahraď skutečným klíčem. Přijímají se přesně 32 znaků
`A-Z` a `2-7`, bez mezer. Uživatel musí být současně v `allowed_user_ids`.
Duplicitní ID, sdílené klíče a neplatné klíče zabrání spuštění. Vynechaná sekce
nebo prázdné pole zachovává běžné příkazy, ale nikdo se nemůže ověřit.
Konfiguraci udržuj čitelnou jen pro vlastníka (`chmod 600 config.json`).
Po změně konfigurace restartuj službu; při ztrátě autentizátoru nahraď klíč
novým lokálně na routeru a znovu spáruj aplikaci.

### Označení chráněného příkazu

V `commands.json` přidej ke konkrétnímu příkazu:

```json
{
  "name": "status",
  "description": "Stav systému",
  "button": {"text": "📊 Stav", "row": 0},
  "script": "scripts/status.sh",
  "requires_2fa": true
}
```

Stejný příznak lze nastavit v C++ při registraci `Command` za položkou
`handler`. Výchozí hodnota je `false`; existující příkazy se samy nezamknou.
Jména `auth` a `lock` jsou vyhrazená a nelze je přepsat vlastním příkazem.

### Použití a platnost

- V soukromém chatu pošli `/auth 123456` s aktuálním kódem z aplikace.
- Úspěšné ověření odemkne všechny chráněné příkazy pro dané `user_id` na
  300 sekund a zobrazí nové menu. Spouštění příkazů dobu neprodlužuje.
  Nové úspěšné ověření novým kódem začne nových 5 minut.
- `/lock` přístup ihned ukončí. Restart bota zruší všechny relace.
- Ověřování i chráněné příkazy fungují pouze v soukromém chatu. Skupiny
  chráněné příkazy nezobrazují a nelze je tam spustit ani po soukromém ověření.
- Bez ověření se chráněný příkaz chová jako neznámý, včetně ručního zadání,
  textu tlačítka a callbacku ze starého menu.
- Po pěti chybných pokusech se ověřování daného uživatele zablokuje na 5 minut.
  `/lock` tento limit nemaže. Uživatelé mají nezávislé limity.
- Bot přijímá aktuální časový krok a sousední kroky (±30 sekund). Již přijatý
  krok a starší kroky znovu nepřijme. Router i telefon potřebují správný čas.
  Délku odemčení a blokace měří monotónní hodiny, takže ji neposune změna
  systémového času. Ochrana proti opakování a limity pokusů jsou v paměti;
  restart je vynuluje, proto může dosud časově platný kód znovu projít.

Bot průběžně odstraňuje chráněná tlačítka ze svého posledního odemčeného menu
po expiraci, zamknutí a při řádném ukončení. Aktualizace Telegram zprávy může
mít zpoždění během běhu skriptu nebo výpadku sítě; po pádu procesu mohou stará
tlačítka zůstat v historii. Kontrola před spuštěním příkazu platí vždy a takové
tlačítko přístup neobnoví. Již odeslané odpovědi zůstávají v historii chatu.

## Lokální notifikace pomocí `tbot-send`

Balíček obsahuje malou binárku `tbot-send`, přes kterou mohou cron nebo vlastní skripty předat zprávu běžícímu daemonu. Klient nečte
Telegram token, nekomunikuje se sítí a nečeká na doručení do Telegramu:

```sh
tbot-send "Router byl restartován"
printf '%s\n' "WAN připojení vypadlo" | tbot-send
```

Při nestandardní cestě k socketu je nutné použít stejnou hodnotu na obou
stranách:

```sh
tbot-send --socket /tmp/tbot-test.sock "Testovací zpráva"
```

Jedna zpráva smí mít nejvýše 3500 bajtů. Daemon drží nejvýše deset čekajících
zpráv; při přetečení zahodí nejstarší a událost zapíše do systémového logu.
Notifikace odesílá nejvýše rychlostí jedné zprávy za sekundu. Fronta je pouze
v paměti a zpráva, kterou Telegram odmítne nebo se ji nepodaří odeslat, se v
této první verzi automaticky neopakuje.

## Založení Telegram bota

1. V Telegramu otevři oficiální účet [@BotFather](https://t.me/BotFather).
2. Odešli `/newbot`.
3. Zvol zobrazované jméno a unikátní username končící na `bot`.
4. BotFather vrátí token. Považuj ho za heslo a nikam ho necommituj.
5. Otevři nového bota a odešli mu `/start`.
6. Zjisti svoje číselné Telegram `user_id` následujícím příkazem ve WSL:

   ```bash
   read -rsp "Telegram bot token: " TELEGRAM_BOT_TOKEN; echo
   curl -s "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/getUpdates" \
     | python3 -m json.tool
   unset TELEGRAM_BOT_TOKEN
   ```

   Ve výsledku najdi `result[].message.from.id`. Toto číslo vlož do
   `telegram.allowed_user_ids`. Username se pro autorizaci nepoužívá.

## Kompatibilita

Výchozí socket `/tmp/tbot-wrt.sock` a TOTP issuer `tbot-wrt` jsou záměrně
zachované kvůli existujícím instalacím. Formát konfigurace a již spárované
2FA klíče se přejmenováním programu nemění. Socket lze změnit konfigurací
bota a odpovídající volbou `tbot-send --socket`.

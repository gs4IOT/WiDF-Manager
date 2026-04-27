# WIDF Manager

A native ESP-IDF WiFi provisioning library for the **ESP32 family**. Developed and tested on the M5Stamp C3 (ESP32-C3). No Arduino framework, no third-party WiFi libraries — built entirely on ESP-IDF components.

Designed as a reusable subsystem: call `widf_mngr_run()` from your `app_main()` and the library handles everything — WiFi init, captive portal, credential storage, STA connection, OTA, mDNS, and portal lifecycle.

---

## Features

- **Single-call integration** — `widf_mngr_run()` takes a config struct and runs the full portal lifecycle; pass `NULL` for defaults
- **Runtime configuration** — AP name, password, security mode, portal timeout, GPIO pin, hostname, NVS namespace, flow control modes, and event callback all configurable at runtime without recompiling
- **Event callbacks** — register a single `on_event` callback to receive all lifecycle events: STA connect/disconnect/fail, portal open/close, and credential save
- **Flow control modes** — three independent mode fields control behavior at each lifecycle transition: on boot failure, on credential save, and on STA connect
- **Multi-network credentials** — up to 3 networks stored in NVS, tried newest-first on boot; new credentials shift existing ones down, oldest dropped at limit
- **No-reboot reconnect** — after credentials are saved via the portal, the WiFi stack reconnects in place without rebooting the device
- **Network scan** — live list of nearby networks with signal strength bars, dBm value, and encryption type
- **Hidden network support** — toggle to reveal hidden APs; SSID entered manually
- **Show/hide password** — toggle button on the password field
- **STA connection with retry** — attempts to connect on boot using saved credentials, configurable retry count per network
- **mDNS** — portal reachable at `widf.local` when connected to a network; hostname configurable at runtime
- **Device info page** — chip model, revision, CPU frequency, flash size, RAM usage, uptime, temperature, and full WiFi status
- **Portal timeout** — web server shuts down automatically after a configurable idle period
- **GPIO long press to reopen** — hold the configured button for the configured duration to trigger a reconnect and reopen the portal without rebooting
- **Headless mode** — set `reopen_gpio = 0` to reboot on portal timeout instead of waiting for a button press
- **OTA firmware update** — browser-based `.bin` upload with real-time progress bar; pre-upload size check; post-write version comparison with downgrade/same-version warning; confirmation page before reboot; automatic rollback if new firmware fails to boot
- **DNS server** — optional Kconfig-gated UDP DNS server on port 53 for captive portal redirect (disabled by default; modern Android uses HTTPS detection which requires TLS)
- **Card-based responsive UI** — mobile-friendly layout, no external CSS frameworks or CDN dependencies
- **Portable** — compiles cleanly on ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C5, ESP32-C6

---

## Quick Start

```c
#include "widf_mngr.h"

void app_main(void)
{
    /* Use all defaults from Kconfig */
    widf_mngr_run(NULL);
}
```

With a custom config and event callback:

```c
#include "widf_mngr.h"
#include "esp_log.h"

static void on_widf_event(const widf_event_data_t *evt)
{
    if (evt->event == WIDF_EVENT_STA_CONNECTED)
        ESP_LOGI("app", "Connected to %s", evt->data.connected.ssid);
}

void app_main(void)
{
    widf_mngr_config_t cfg = WIDF_MNGR_DEFAULT_CONFIG();
    strncpy(cfg.ap_ssid,  "MyDevice-Setup", sizeof(cfg.ap_ssid));
    strncpy(cfg.hostname, "mydevice",        sizeof(cfg.hostname));
    cfg.on_connect_mode = WIDF_ON_CONNECT_PORTAL_STA;
    cfg.reopen_gpio     = 9;
    cfg.on_event        = on_widf_event;
    widf_mngr_run(&cfg);
}
```

Once connected to a network, the portal is reachable at:
- `widf.local` (or your configured hostname + `.local`) via mDNS
- The device's STA IP shown on the `/info` page

---

## Examples

Three examples are provided under `components/widf_mngr/examples/`:

### `basic`

Minimal integration — single call with all Kconfig defaults. The starting point for any new project.

```c
#include "widf_mngr.h"

void app_main(void)
{
    widf_mngr_run(NULL);
}
```

### `custom_config`

Demonstrates runtime configuration using `widf_mngr_config_t` and all three flow control mode fields. Shows how to customise the AP name, hostname, GPIO pin, portal timeout, and lifecycle behavior without recompiling.

### `event_callbacks`

Demonstrates the event callback system. Registers an `on_event` handler that receives all seven lifecycle events — STA trying, connected, disconnected, failed, portal opened, portal closed, and credentials saved — with comments explaining when each fires and what data is available.

---

## Public API

### `widf_mngr_run()`

```c
esp_err_t widf_mngr_run(const widf_mngr_config_t *config);
```

Initialises NVS, GPIO, and WiFi, then runs the full portal lifecycle. Blocks indefinitely — call from `app_main()` or a dedicated task. Pass `NULL` to use all Kconfig defaults. Returns `ESP_OK` if `fallback_mode` is `WIDF_FALLBACK_EVENT_ONLY` and no connection is available.

### `widf_mngr_get_sta_ip()`

```c
const char *widf_mngr_get_sta_ip(void);
```

Returns the current STA IP address as a string, or `NULL` if not connected. Valid while `widf_mngr_run()` is executing.

### `widf_mngr_config_t`

```c
typedef struct {
    /* AP */
    char              ap_ssid[32];       /* Portal AP network name                        */
    char              ap_password[64];   /* AP password — empty string = open             */
    wifi_auth_mode_t  ap_authmode;       /* WIFI_AUTH_OPEN / WPA2_PSK / WPA3_PSK         */

    /* Portal */
    uint32_t          portal_timeout_s;  /* Seconds before portal closes                  */
    int               reopen_gpio;       /* 0 = reboot on timeout (headless)              */
                                         /* >0 = GPIO pin number for long press           */
    uint32_t          long_press_ms;     /* Long press duration in ms                     */

    /* Device */
    char              hostname[32];      /* mDNS / DHCP hostname (default: "widf")        */
    char              nvs_namespace[16]; /* NVS namespace for credential storage          */
    uint8_t           max_sta_retries;   /* STA connection retry count per network        */

    /* Flow control */
    widf_fallback_mode_t   fallback_mode;   /* Behavior when no network connects on boot  */
    widf_on_save_mode_t    on_save_mode;    /* Behavior after credentials saved            */
    widf_on_connect_mode_t on_connect_mode; /* Behavior after STA connects successfully   */

    /* Events */
    widf_event_cb_t   on_event;          /* NULL = no callback                            */
} widf_mngr_config_t;
```

### `WIDF_MNGR_DEFAULT_CONFIG()`

Macro that initialises a `widf_mngr_config_t` with all Kconfig defaults. Use this as a starting point and override only the fields you need.

---

## Flow Control Modes

Three independent mode fields control behavior at key lifecycle transitions.

### `widf_fallback_mode_t` — on boot failure

Controls what happens when no saved network connects on boot (no credentials stored, or all connection attempts failed).

| Value | Behavior |
|-------|----------|
| `WIDF_FALLBACK_AP_PORTAL` | Start AP and open portal (default) |
| `WIDF_FALLBACK_EVENT_ONLY` | Fire `WIDF_EVENT_STA_FAILED` and return `ESP_OK` to host |

### `widf_on_save_mode_t` — after credentials saved

Controls what happens after the user submits credentials via the portal.

| Value | Behavior |
|-------|----------|
| `WIDF_ON_SAVE_RECONNECT` | Close portal, reconnect WiFi stack in place — no reboot (default) |
| `WIDF_ON_SAVE_EVENT_ONLY` | Fire `WIDF_EVENT_CREDENTIALS_SAVED`, portal stays open, host decides |

### `widf_on_connect_mode_t` — after STA connects

Controls what happens after a successful STA connection.

| Value | Behavior |
|-------|----------|
| `WIDF_ON_CONNECT_AP_DOWN` | AP comes down, portal closes (default) |
| `WIDF_ON_CONNECT_PORTAL_STA` | AP comes down, portal stays running on STA IP |
| `WIDF_ON_CONNECT_EVENT_ONLY` | Fire `WIDF_EVENT_STA_CONNECTED`, host decides |

---

## Event Callbacks

Register a single callback via `cfg.on_event` to receive all lifecycle events. Set to `NULL` to disable.

```c
typedef void (*widf_event_cb_t)(const widf_event_data_t *event);
```

### Events

| Event | When fired | Data available |
|-------|-----------|----------------|
| `WIDF_EVENT_STA_TRYING` | Before connect attempt, once per network | `ssid`, `retries` |
| `WIDF_EVENT_STA_CONNECTED` | STA got IP | `ssid`, `ip` |
| `WIDF_EVENT_STA_DISCONNECTED` | Mid-session drop (not a failed connect) | `ssid` |
| `WIDF_EVENT_STA_FAILED` | All retries exhausted for a network | `ssid`, `retries` |
| `WIDF_EVENT_PORTAL_OPENED` | HTTP server started | — |
| `WIDF_EVENT_PORTAL_CLOSED` | HTTP server stopped | — |
| `WIDF_EVENT_CREDENTIALS_SAVED` | New credentials written to NVS | `ssid` |

The callback runs in the `widf_mngr` task context — keep it non-blocking.

---

## Multi-Network Credentials

Up to 3 networks are stored in NVS under indexed keys (`ssid_0/pass_0` through `ssid_2/pass_2`). On boot, networks are tried newest-first. Each network gets `max_sta_retries` attempts before the next is tried.

When new credentials are saved:
- If the SSID already exists — password is updated in place
- If it is a new SSID — existing entries shift down, new entry written at index 0
- If 3 networks are already stored — the oldest is dropped

> **Migration note:** v1.2.0 uses a new NVS key format. Credentials saved by earlier versions are not migrated automatically. Erasing the NVS partition (`idf.py erase-flash`) before first use of v1.2.0 is recommended.

---

## Architecture

### File structure

```
WiDF-Manager/
├── components/
│   └── widf_mngr/
│       ├── examples/
│       │   ├── basic/               # Minimal integration — single call, all defaults
│       │   ├── custom_config/       # Runtime config and flow control modes
│       │   └── event_callbacks/     # Event callback system — all seven events
│       ├── include/
│       │   └── widf_mngr.h          # Public API header
│       ├── widf_mngr_main.c         # System core — NVS, WiFi, scan, widf_mngr_run()
│       ├── widf_mngr_handlers.c     # All HTTP handlers and start_webserver()
│       ├── widf_mngr_handlers.h     # Handler declarations, PAGE_HEAD macro, shared externs
│       ├── widf_mngr_dns.c          # Optional UDP DNS server (Kconfig gated)
│       ├── widf_mngr_dns.h          # dns_server_start() / dns_server_stop()
│       ├── Top_portal.html          # WiFi setup page — top half (embedded into firmware)
│       ├── Bottom_portal.html       # WiFi setup page — bottom half + JavaScript
│       ├── CMakeLists.txt           # Component registration, source files, embedded HTML
│       ├── idf_component.yml        # IDF Component Manager manifest
│       └── Kconfig                  # menuconfig options (build-time defaults)
├── main/
│   └── main.c                       # Entry point — calls widf_mngr_run()
├── CMakeLists.txt                   # Project-level build config
├── partitions.csv                   # 4MB OTA partition table
├── dependencies.lock                # Locked component versions
└── README.md
```

### Design decisions

**Library-first architecture.** `widf_mngr_run()` owns the full lifecycle — NVS init, WiFi driver, AP config, STA connect, portal loop, mDNS, and GPIO handling. The host application's `app_main()` fills a config struct and calls it. The component does not define `app_main()`. Kconfig options serve as build-time defaults that the config struct can override at runtime.

**Static config pointer.** `s_cfg` is set at the start of `widf_mngr_run()` and used by all internal functions. This avoids threading config through every helper call while keeping the public API clean.

**Two C files, one public header.** `widf_mngr_main.c` owns the system layer. `widf_mngr_handlers.c` owns the web layer. `widf_mngr.h` is the only header the host application needs to include.

**Split HTML files.** The WiFi setup page (`/wifi`) injects dynamic content between two static chunks. `Top_portal.html` and `Bottom_portal.html` are embedded directly into the firmware binary at build time using `EMBED_TXTFILES`. No filesystem or SPIFFS partition required.

**`portal_run()` polling loop.** The portal runs inside a 500ms polling loop that checks `g_exit_requested` and `g_reconnect_requested` alongside the timeout counter. This allows the `/exit` route and the reconnect flag to signal a clean server shutdown without deadlocking — flags are set inside handlers, detected by the loop, and `httpd_stop()` is called safely from the main task context.

**No-reboot reconnect.** When `on_save_mode == WIDF_ON_SAVE_RECONNECT`, the portal closes and `try_all_networks()` reconfigures the WiFi stack with the new credentials — no `esp_restart()`. The reconnect flag is set by `save_post_handler()` and polled by `portal_run()`, keeping handler and lifecycle code cleanly separated.

**Multi-network connect loop.** `try_all_networks()` loads all stored networks from NVS and calls `try_sta_connect()` for each in order. `try_sta_connect()` handles a single network including retries, event firing, and event group synchronization. This separation keeps each function focused and testable.

**OTA via browser upload.** `ota_upload_handler` receives the raw `.bin` file as a POST body, checks `Content-Length` against partition capacity, writes via `esp_ota_ops`, compares versions, and sets the boot partition. `esp_ota_mark_app_valid_cancel_rollback()` is called at the start of `widf_mngr_run()` so the bootloader knows the firmware booted successfully.

**mDNS via espressif/mdns.** `mdns_start()` is called after a successful STA connection. The hostname is taken from `config->hostname` (default `"widf"`), making the portal reachable at `widf.local` without knowing the device's IP address.

### Boot sequence

```
widf_mngr_run(&cfg)
  │
  ├─ OTA rollback cancel (mark boot valid)
  ├─ NVS init
  ├─ GPIO init (if reopen_gpio > 0)
  ├─ WiFi init — APSTA mode, AP configured from cfg
  ├─ esp_wifi_start()
  │
  ├─ try_all_networks() — load up to 3 networks from NVS, try newest-first
  │     Network found + connected → handle_connect_result()
  │       WIDF_ON_CONNECT_AP_DOWN    → AP down, portal on demand only
  │       WIDF_ON_CONNECT_PORTAL_STA → AP down, portal on STA IP
  │       WIDF_ON_CONNECT_EVENT_ONLY → fire event, host decides
  │     All networks failed / no credentials →
  │       WIDF_FALLBACK_AP_PORTAL  → open portal
  │       WIDF_FALLBACK_EVENT_ONLY → return ESP_OK to host
  │
  ├─┐ portal_run() ←────────────────────────────────────────────┐
  │ ├─ wifi_scan() — blocking, populates scan buffer             │
  │ ├─ [dns_server_start() if PORTAL_DNS_ENABLED]                │
  │ ├─ start_webserver() — 13 routes registered                  │
  │ ├─ WIDF_EVENT_PORTAL_OPENED fired                            │
  │ ├─ Poll loop every 500ms:                                    │
  │ │     timeout reached → break                                │
  │ │     g_exit_requested (/exit) → break                       │
  │ │     g_reconnect_requested (credentials saved) → break      │
  │ ├─ httpd_stop()                                              │
  │ ├─ WIDF_EVENT_PORTAL_CLOSED fired                            │
  │ └─ [dns_server_stop() if PORTAL_DNS_ENABLED]                 │
  │                                                              │
  ├─ g_reconnect_requested? → try_all_networks() → continue loop │
  │                                                              │
  ├─ reopen_gpio == 0? → esp_restart() (headless)                │
  │                                                              │
  ├─ wait_for_long_press() — polls GPIO every 50ms               │
  │     held ≥ long_press_ms → continue                          │
  │                                                              │
  ├─ try_all_networks() with saved credentials                   │
  │     Connected → handle_connect_result()                      │
  └─────────────────────────────────────────────────────────→───┘
     (loops forever)
```

---

## Build and Flash

### Prerequisites

- ESP-IDF v6.x installed and environment sourced
- 4MB flash (confirmed via `idf.py flash-id` or board specs)

### Setup

```bash
idf.py set-target esp32c3   # change to your target: esp32, esp32s2, esp32s3, esp32c5, esp32c6
idf.py menuconfig            # set flash size to 4MB, set PORTAL_BUTTON_GPIO for your board
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### GPIO pin by board

| Board | Button GPIO |
|-------|-------------|
| M5Stamp C3 | 3 (default) |
| ESP32 DevKit | 0 |
| ESP32-C6 DevKit | 9 |
| ESP32-S3 DevKit | 0 |

### Partition table

`partitions.csv` defines a custom 4MB OTA layout giving **1792KB per OTA slot** — more headroom than the built-in preset (1280KB per slot). To use it, set `Partition Table → Custom partition table CSV` in menuconfig and enter `partitions.csv`.

If switching from a single-partition layout, erase flash first:

```bash
idf.py -p /dev/ttyACM0 erase-flash
idf.py -p /dev/ttyACM0 flash monitor
```

> This wipes saved WiFi credentials — reconfigure via the portal after flashing.

### Clean build

```bash
idf.py fullclean && idf.py build
```

---

## Configuration

Run `idf.py menuconfig` → **WIDF Manager Configuration** for build-time defaults. All options can be overridden at runtime via `widf_mngr_config_t`.

| Option | Default | Description |
|--------|---------|-------------|
| `PORTAL_AP_SSID` | `WIDF-MANAGER` | SSID broadcast by the portal AP |
| `PORTAL_AP_CHANNEL` | `3` | WiFi channel for the AP (1–13) |
| `PORTAL_MAX_STA_CONN` | `5` | Maximum simultaneous clients on the portal AP |
| `PORTAL_TIMEOUT_S` | `180` | Seconds before the portal auto-closes |
| `PORTAL_MAX_RETRY` | `3` | STA connection attempts per network before trying the next |
| `PORTAL_BUTTON_GPIO` | `3` | GPIO pin for the active-low portal reopen button |
| `PORTAL_LONG_PRESS_MS` | `3000` | Hold duration in ms to trigger portal reopen |
| `PORTAL_DNS_ENABLED` | `n` | Enable UDP DNS server on port 53 (see DNS note above) |

---

## Portal Pages

| Route | Method | Description |
|-------|--------|-------------|
| `/` | GET | Main menu — live connection status and navigation |
| `/wifi` | GET | WiFi setup — network list, SSID field, password field |
| `/wifi/refresh` | GET | Re-scan networks and redirect back to `/wifi` |
| `/save` | POST | Save credentials to NVS; behavior controlled by `on_save_mode` |
| `/info` | GET | Live device info — chip, memory, uptime, temperature, WiFi details, route table |
| `/erase` | GET | Erase all WiFi credentials from NVS and reboot |
| `/ota` | GET | OTA firmware update — file picker with real-time progress bar |
| `/ota/upload` | POST | Receive `.bin`; size check; write to inactive partition; set boot target |
| `/restart` | GET | Reboot the device |
| `/exit` | GET | Shut down the portal; long press the configured button to reopen |
| `/generate_204` | GET | Android captive portal redirect to `192.168.4.1` |
| `/favicon.ico` | GET | Returns 204 No Content — suppresses browser icon request noise |

The portal is reachable at:
- `192.168.4.1` — while connected to the AP
- `widf.local` — when connected to a network (mDNS, requires same network)
- Device STA IP — when `on_connect_mode` is `WIDF_ON_CONNECT_PORTAL_STA`

---

## Compatibility

Compiled and verified on all major ESP32 targets:

| Target | Architecture | Status |
|--------|-------------|--------|
| ESP32-C3 | RISC-V | ✅ Primary dev board |
| ESP32-C5 | RISC-V | ✅ Verified |
| ESP32-C6 | RISC-V | ✅ Verified |
| ESP32 | Xtensa | ✅ Verified |
| ESP32-S2 | Xtensa | ✅ Verified |
| ESP32-S3 | Xtensa | ✅ Verified |

Temperature sensor display is conditionally compiled (`CONFIG_SOC_TEMP_SENSOR_SUPPORTED`) and shows `N/A` on unsupported chips. WPA3/OWE auth modes are guarded by `CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT` and `CONFIG_ESP_WIFI_OWE_SUPPORT`.

---

## Version History

| Version | Notes |
|---------|-------|
| v1.2.0 | README overhaul; `event_callbacks` example added; `custom_config` updated for flow control modes |
| v1.1.3 | Multi-network credentials — 3 slots, indexed NVS (`ssid_0/pass_0`), newest-first connect order, per-network retries |
| v1.1.2 | Flow control enums (`widf_fallback_mode_t`, `widf_on_save_mode_t`, `widf_on_connect_mode_t`); WiFi stack reconnect replaces `esp_restart()`; `app_main()` removed from component |
| v1.1.1 | Event callbacks — `widf_event_t`, `widf_event_data_t`, `widf_event_cb_t`, `on_event` config field |
| v1.0.0 | Component structure (`components/widf_mngr/`), public header in `include/`, examples: `basic` and `custom_config` |
| v0.9.1 | mDNS (`widf.local`), favicon handler (204), `/generate_204` captive redirect, route table complete (13 routes), buffer audit |
| v0.9.0 | Public API: `widf_mngr_run()`, `widf_mngr_config_t`, `sta_access` mode, runtime config struct, `widf_mngr_get_sta_ip()` |
| v0.8.5 | DNS server (Kconfig gated), captive portal redirect, scan deduplication, route table fix |
| v0.8.0 | OTA upload with progress bar, version comparison, A/B partition swap, rollback protection |
| v0.7.5 | IDF version string on `/info`, build date in OTA page, portability guards |
| v0.7.0 | OTA browser upload, 4MB partition table, `PORTAL_MAX_RETRY` in Kconfig |
| v0.6.0 | Board-portable GPIO long press, `portal_run()` loop, `try_sta_connect()` refactor, `/exit` flag pattern |
| v0.5.0 | Split into main + handlers files, full info page, menu page, all routes wired |
| v0.4.x | Single-file portal, NVS credentials, STA connect with retry, UI redesign |
| v0.1–0.3 | SoftAP example base, NVS helpers, HTTP server, WiFi scan |

---

## License

This project is licensed under the **GNU General Public License v3.0**.

You are free to use, modify, and distribute this software, provided that any distributed modifications or derivative works are also released under GPL v3 with full source code. See the `LICENSE` file for the full license text or visit [gnu.org/licenses/gpl-3.0](https://www.gnu.org/licenses/gpl-3.0).

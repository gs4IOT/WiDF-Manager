# WIDF Manager

A native ESP-IDF WiFi provisioning library for the **ESP32 family**. Developed and tested on the M5Stamp C3 (ESP32-C3). No Arduino framework, no third-party WiFi libraries — built entirely on ESP-IDF components.

Designed as a reusable subsystem: call `widf_mngr_run()` from your `app_main()` and the library handles everything — WiFi init, captive portal, credential storage, STA connection, OTA, mDNS, and portal lifecycle.

---

## Features

- **Single-call integration** — `widf_mngr_run()` takes a config struct and runs the full portal lifecycle; pass `NULL` for defaults
- **Runtime configuration** — AP name, password, security mode, portal timeout, GPIO pin, access mode, hostname, and NVS namespace all configurable at runtime without recompiling
- **Network scan** — live list of nearby networks with signal strength bars, dBm value, and encryption type
- **Hidden network support** — toggle to reveal hidden APs; SSID entered manually
- **Show/hide password** — toggle button on the password field
- **Credential storage** — SSID and password saved to NVS flash, survives reboots and power cycles
- **STA connection with retry** — attempts to connect on boot using saved credentials, configurable retry count
- **Two access modes** — `sta_access` flag controls portal and AP behavior after STA connects (see Access Modes below)
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

With a custom config:

```c
#include "widf_mngr.h"

void app_main(void)
{
    widf_mngr_config_t cfg = WIDF_MNGR_DEFAULT_CONFIG();
    strncpy(cfg.ap_ssid,  "MyDevice-Setup", sizeof(cfg.ap_ssid));
    strncpy(cfg.hostname, "mydevice",       sizeof(cfg.hostname));
    cfg.sta_access  = true;   /* portal stays on STA IP after connect  */
    cfg.reopen_gpio = 9;      /* GPIO9 long press to reopen portal      */
    widf_mngr_run(&cfg);
}
```

Once connected to a network, the portal is reachable at:
- `widf.local` (or your configured hostname + `.local`) via mDNS
- The device's STA IP shown on the `/info` page

---

## Public API

### `widf_mngr_run()`

```c
esp_err_t widf_mngr_run(const widf_mngr_config_t *config);
```

Initialises NVS, GPIO, and WiFi, then runs the full portal lifecycle. Blocks indefinitely — call from `app_main()` or a dedicated task. Pass `NULL` to use all Kconfig defaults.

### `widf_mngr_get_sta_ip()`

```c
const char *widf_mngr_get_sta_ip(void);
```

Returns the current STA IP address as a string, or `NULL` if not connected. Valid while `widf_mngr_run()` is executing.

### `widf_mngr_config_t`

```c
typedef struct {
    char             ap_ssid[32];       /* Portal AP network name                  */
    char             ap_password[64];   /* AP password — empty string = open       */
    wifi_auth_mode_t ap_authmode;       /* WIFI_AUTH_OPEN / WPA2_PSK / WPA3_PSK   */
    uint32_t         portal_timeout_s;  /* Seconds before portal closes            */
    int              reopen_gpio;       /* 0 = reboot on timeout (headless)        */
                                        /* >0 = GPIO pin number for long press     */
    uint32_t         long_press_ms;     /* Long press duration in ms               */
    bool             sta_access;        /* See Access Modes below                  */
    char             hostname[32];      /* mDNS / DHCP hostname (default: "widf")  */
    char             nvs_namespace[16]; /* NVS namespace for credential storage    */
    uint8_t          max_sta_retries;   /* STA connection retry count              */
} widf_mngr_config_t;
```

### `WIDF_MNGR_DEFAULT_CONFIG()`

Macro that initialises a `widf_mngr_config_t` with all Kconfig defaults. Use this as a starting point and override only the fields you need.

---

## Access Modes

`sta_access` controls what happens after a successful STA connection:

**`sta_access = true`** (default — networked device)
- Portal HTTP server stays running, reachable at the device's STA IP and at `widf.local`
- AP shuts down after STA connects — no separate AP network needed
- OTA, configuration, and all portal routes accessible from the local network
- Use this when the device lives on a network and you want ongoing browser access

**`sta_access = false`** (on-demand / minimal)
- Portal only runs when triggered by GPIO long press or reboot
- On trigger: AP starts if not already up, portal opens
- Portal and AP shut down together after timeout
- Use this for devices where configuration is rare and RF footprint matters

---

## Architecture

### File structure

```
WiDF-Manager/
├── components/
│   └── widf_mngr/
│       ├── examples/
│       │   ├── basic/               # Minimal integration example
│       │   └── custom_config/       # Runtime config struct example
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

**Library-first architecture.** `widf_mngr_run()` owns the full lifecycle — NVS init, WiFi driver, AP config, STA connect, portal loop, mDNS, and GPIO handling. The host application's `app_main()` fills a config struct and calls it. Kconfig options serve as build-time defaults that the config struct can override at runtime.

**Static config pointer.** `s_cfg` is set at the start of `widf_mngr_run()` and used by all internal functions. This avoids threading config through every helper call while keeping the public API clean.

**Two C files, one public header.** `widf_mngr_main.c` owns the system layer. `widf_mngr_handlers.c` owns the web layer. `widf_mngr.h` is the only header the host application needs to include.

**Split HTML files.** The WiFi setup page (`/wifi`) injects dynamic content between two static chunks. `Top_portal.html` and `Bottom_portal.html` are embedded directly into the firmware binary at build time using `EMBED_TXTFILES`. No filesystem or SPIFFS partition required.

**`portal_run()` polling loop.** The portal runs inside a 500ms polling loop that checks `g_exit_requested` alongside the timeout counter. This allows the `/exit` route to signal a clean server shutdown without deadlocking — the flag is set inside the handler, detected by the loop, and `httpd_stop()` is called safely from the main task context.

**OTA via browser upload.** `ota_upload_handler` receives the raw `.bin` file as a POST body, checks `Content-Length` against partition capacity, writes via `esp_ota_ops`, compares versions, and sets the boot partition. `esp_ota_mark_app_valid_cancel_rollback()` is called at the start of `widf_mngr_run()` so the bootloader knows the firmware booted successfully — if never called, the bootloader rolls back to the previous working image.

**mDNS via espressif/mdns.** `mdns_start()` is called after a successful STA connection. The hostname is taken from `config->hostname` (default `"widf"`), making the portal reachable at `widf.local` without knowing the device's IP address.

**DNS server (optional).** A UDP port 53 server that responds to all A-record queries with `192.168.4.1`. Gated by `PORTAL_DNS_ENABLED` (default off). Modern Android uses HTTPS for captive portal detection and will not show an automatic popup — users navigate to `widf.local` or `192.168.4.1` manually.

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
  ├─ Saved credentials found?
  │     YES → try_sta_connect() — configure STA, connect, wait for IP or failure
  │           Connected → mdns_start() — portal reachable at widf.local
  │           Connected + sta_access=true  → AP down, portal on STA IP
  │           Connected + sta_access=false → AP down, portal on demand only
  │     NO  → open portal directly
  │
  ├─┐ portal_run() ←─────────────────────────────────────┐
  │ ├─ wifi_scan() — blocking, populates scan buffer      │
  │ ├─ [dns_server_start() if PORTAL_DNS_ENABLED]         │
  │ ├─ start_webserver() — 13 routes registered           │
  │ ├─ Poll loop every 500ms:                             │
  │ │     timeout reached → break                         │
  │ │     g_exit_requested set by /exit → break           │
  │ ├─ httpd_stop()                                       │
  │ └─ [dns_server_stop() if PORTAL_DNS_ENABLED]          │
  │                                                       │
  ├─ reopen_gpio == 0? → esp_restart() (headless)         │
  │                                                       │
  ├─ wait_for_long_press() — polls GPIO every 50ms        │
  │     held ≥ long_press_ms → continue                   │
  │                                                       │
  ├─ try_sta_connect() with saved credentials             │
  │     Connected → mdns_start()                          │
  └────────────────────────────────────────────────────→──┘
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
| `PORTAL_MAX_RETRY` | `3` | STA connection attempts before opening the portal |
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
| `/save` | POST | Save submitted SSID and password to NVS, then reboot |
| `/info` | GET | Live device info — chip, memory, uptime, temperature, WiFi details, route table |
| `/erase` | GET | Erase WiFi credentials from NVS and reboot |
| `/ota` | GET | OTA firmware update — file picker with real-time progress bar |
| `/ota/upload` | POST | Receive `.bin`; size check; write to inactive partition; set boot target |
| `/restart` | GET | Reboot the device |
| `/exit` | GET | Shut down the portal; long press the configured button to reopen |
| `/generate_204` | GET | Android captive portal redirect to `192.168.4.1` |
| `/favicon.ico` | GET | Returns 204 No Content — suppresses browser icon request noise |

The portal is reachable at:
- `192.168.4.1` — while connected to the AP
- `widf.local` — when connected to a network (mDNS, requires same network)
- Device STA IP — when `sta_access = true` and STA is connected

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

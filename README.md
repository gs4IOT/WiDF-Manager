# WIDF Manager

A native ESP-IDF captive portal and WiFi configuration manager for the **ESP32 family**. Developed and tested on the M5Stamp C3 (ESP32-C3). No Arduino framework, no third-party WiFi libraries — built entirely on ESP-IDF components.

---

## Features

- **Captive portal** — portal AP broadcasts on `192.168.4.1`; DNS redirect for auto-pop is planned but not yet implemented
- **Network scan** — live list of nearby networks with signal strength bars, dBm value, and encryption type
- **Hidden network support** — toggle to reveal hidden APs; SSID entered manually
- **Show/hide password** — toggle button on the password field
- **Credential storage** — SSID and password saved to NVS flash, survives reboots and power cycles
- **STA connection with retry** — attempts to connect on boot using saved credentials, retries up to `PORTAL_MAX_RETRY` times before falling through to the portal
- **APSTA mode** — portal AP and STA connection run simultaneously; portal remains accessible while connected
- **Device info page** — chip model, revision, CPU frequency, flash size, RAM usage, uptime, temperature, and full WiFi status
- **Portal timeout** — web server shuts down automatically after a configurable idle period
- **GPIO long press to reopen** — hold the configured button (`PORTAL_BUTTON_GPIO`) for the configured duration (`PORTAL_LONG_PRESS_MS`) after the portal closes to trigger a reconnect attempt and reopen the portal without rebooting
- **Reboot / erase / exit** — utility routes for device management directly from the browser
- **OTA firmware update** — browser-based `.bin` upload with real-time progress bar; pre-upload size check against partition capacity; post-write version comparison with downgrade/same-version warning; confirmation page before reboot; automatic rollback if new firmware fails to boot
- **Configurable AP behaviour** — optionally shut down the portal AP after a successful STA connection (`PORTAL_KEEP_AP_ACTIVE=n`) and optionally reboot on portal timeout instead of waiting for a long press (`PORTAL_REBOOT_ON_TIMEOUT=y`; useful for headless deployments)
- **Card-based responsive UI** — mobile-friendly layout, no external CSS frameworks or CDN dependencies

---

## Architecture

### File structure

```
widf_mngr/
├── main/
│   ├── widf_mngr_main.c       # Boot sequence, WiFi init, NVS load, scan, app_main
│   ├── widf_mngr_handlers.c   # All HTTP handlers and start_webserver()
│   ├── widf_mngr_handlers.h   # Handler declarations, PAGE_HEAD macro, shared externs
│   ├── Top_portal.html        # WiFi setup page — top half (embedded into firmware)
│   ├── Bottom_portal.html     # WiFi setup page — bottom half + JavaScript
│   ├── CMakeLists.txt         # Component registration, source files, embedded HTML
│   └── Kconfig.projbuild      # menuconfig options
├── CMakeLists.txt             # Project-level build config
├── partitions.csv             # 4MB OTA partition table
└── README.md
```

### Design decisions

**Two C files, one header.** `widf_mngr_main.c` owns the system layer — NVS, WiFi driver, scan, event handler, and `app_main`. It does not know HTTP. `widf_mngr_handlers.c` owns the web layer — one function per route plus `start_webserver()`. The header exposes the scan buffer and handler declarations as the bridge between them.

**Split HTML files.** The WiFi setup page (`/wifi`) injects dynamic content between two static chunks: a live status banner and the scanned network list. `Top_portal.html` and `Bottom_portal.html` are embedded directly into the firmware binary at build time using `EMBED_TXTFILES`. No filesystem or SPIFFS partition required.

**Inline pages for simple routes.** Pages like `/info`, `/restart`, `/erase`, and `/exit` are built entirely in C using `snprintf` and `httpd_resp_sendstr_chunk`. The `PAGE_HEAD(title)` macro in the header provides shared CSS so all simple pages look consistent without duplicating styles.

**APSTA mode and configurable AP shutdown.** By default the device never switches to pure STA mode — the portal AP stays up even after a successful STA connection, making reconfiguration possible at any time without a reboot. Setting `PORTAL_KEEP_AP_ACTIVE=n` in menuconfig switches the device to `WIFI_MODE_STA` after a successful connection, reducing RF footprint and power consumption. The long-press button still reopens the portal regardless.

**`portal_run()` loop instead of blocking delay.** The portal runs inside a polling loop that checks `g_exit_requested` every 500ms alongside the timeout counter. This means the `/exit` route can signal a clean server shutdown without a reboot — the handler sets the flag, the loop detects it, and `httpd_stop()` is called safely from `app_main` context rather than from inside the handler (which would deadlock). Setting `PORTAL_REBOOT_ON_TIMEOUT=y` causes the device to reboot automatically on timeout instead of entering the long-press wait loop — useful for headless deployments.

**OTA via browser upload.** `ota_upload_handler` receives the raw `.bin` file as a POST body. Before writing, it checks that `Content-Length` is present and that the binary fits within the target partition — oversized uploads are rejected immediately with a clear error. After writing, it reads the new firmware's app description and compares versions: same version and downgrades show an amber warning on the confirmation page, upgrades proceed silently. The handler then sets the new partition as the next boot target. `esp_ota_mark_app_valid_cancel_rollback()` is called at the start of `app_main` so the bootloader knows the firmware booted successfully — if it is never called, the bootloader rolls back to the previous working image automatically.

**GPIO long press via polling.** `wait_for_long_press()` polls the configured button pin (`PORTAL_BUTTON_GPIO`) every 50ms using `vTaskDelay` between checks, accumulating held time and resetting on release. Simple and reliable for a human-speed interaction.

### Boot sequence

```
Power on
  │
  ├─ NVS init (erase + reinit if corrupted or version mismatch)
  ├─ GPIO init — configured pin, input, pull-up, active low
  ├─ WiFi init — APSTA mode, AP configured
  ├─ esp_wifi_start()
  │
  ├─ Saved credentials found?
  │     YES → try_sta_connect() — configure STA, connect, wait for IP or failure
  │     NO  → skip, open portal directly
  │
  ├─┐ portal_run() ←──────────────────────────────────┐
  │ ├─ wifi_scan() — blocking, populates scan buffer   │
  │ ├─ start_webserver() — 10 routes registered        │
  │ ├─ Poll loop every 500ms:                          │
  │ │     timeout reached → break                      │
  │ │     g_exit_requested set by /exit → break        │
  │ └─ httpd_stop()                                    │
  │                                                    │
  ├─ wait_for_long_press() — polls button pin every 50ms │
  │     held ≥ 3s → continue                           │
  │                                                    │
  ├─ try_sta_connect() with saved credentials          │
  └──────────────────────────────────────────────────→─┘
     (loops forever)
```

---

## Build and Flash

### Prerequisites

- ESP-IDF v6.x installed and environment sourced
- Target: any ESP32 variant (tested on ESP32-C3 / M5Stamp C3)

### First time setup

```bash
idf.py set-target esp32c3   # change to your target, e.g. esp32s3, esp32c6
idf.py menuconfig        # set Flash size to 4MB, select partitions.csv
idf.py build
```

### Partition table

This project ships with `partitions.csv` which defines a custom 4MB OTA layout giving **1792KB per slot** — significantly more than the built-in "Two large OTA partitions" preset (1280KB per slot) available in `idf.py menuconfig`. If your board has exactly 4MB flash and you want more headroom for app growth, use `partitions.csv`. If you prefer not to manage a custom file, the built-in preset works too but leaves less room.

To use `partitions.csv`, set `Partition Table → Custom partition table CSV` in menuconfig and enter `partitions.csv` as the filename.

### First flash after partition table change

If switching from a single-partition layout to the OTA layout, erase flash first:

```bash
idf.py -p /dev/ttyACM0 erase-flash
idf.py -p /dev/ttyACM0 flash monitor
```

> This wipes saved WiFi credentials — you will need to reconfigure via the portal after flashing.

### Flash and monitor

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

> If flashing fails to connect, hold the **G3 button** while pressing **Reset** to force the chip into download mode, then retry.

### Clean build

If you change `Kconfig.projbuild` or `CMakeLists.txt` and get stale build errors:

```bash
idf.py fullclean
idf.py build
```

---

## Configuration

All options are in **WIDF Manager Configuration** — run `idf.py menuconfig` to access them.

| Option | Default | Range | Description |
|--------|---------|-------|-------------|
| `PORTAL_AP_SSID` | `WIDF-MANAGER` | string | SSID broadcast by the portal access point |
| `PORTAL_AP_CHANNEL` | `3` | 1–13 | WiFi channel for the AP |
| `PORTAL_MAX_STA_CONN` | `5` | 1–10 | Maximum simultaneous clients on the portal AP |
| `PORTAL_TIMEOUT_S` | `180` | 30–600 | Seconds before the web server auto-shuts down |
| `PORTAL_MAX_RETRY` | `3` | 1–10 | STA connection attempts before opening the portal |
| `PORTAL_BUTTON_GPIO` | `3` | 0–48 | GPIO pin for the active-low portal reopen button (default: G3 on M5Stamp C3) |
| `PORTAL_LONG_PRESS_MS` | `3000` | 1000–10000 | Hold duration in ms to trigger a portal reopen |
| `PORTAL_KEEP_AP_ACTIVE` | `y` | bool | Keep portal AP up after STA connects; disable to switch to pure STA mode |
| `PORTAL_REBOOT_ON_TIMEOUT` | `n` | bool | Reboot on portal timeout instead of waiting for long press; for headless use |


---

## Portal Pages

| Route | Method | Description |
|-------|--------|-------------|
| `/` | GET | Main menu — live connection status and navigation |
| `/wifi` | GET | WiFi setup — network list, SSID field, password field |
| `/wifi/refresh` | GET | Re-scan networks and redirect back to `/wifi` |
| `/save` | POST | Save submitted SSID and password to NVS, then reboot |
| `/info` | GET | Live device info — chip, memory, uptime, temperature, WiFi details |
| `/erase` | GET | Erase WiFi credentials from NVS and reboot |
| `/ota` | GET | OTA firmware update — file picker with progress bar |
| `/ota/upload` | POST | Receive raw `.bin`; checks size and version; writes to inactive partition; set boot target |
| `/restart` | GET | Reboot the device |
| `/exit` | GET | Shut down the portal; long press the configured button to reopen |

The portal is reachable at `192.168.4.1` while connected to the AP. DNS auto-redirect is not yet implemented — users need to navigate to `192.168.4.1` manually in their browser.

---

## Version History

| Version | Notes |
|---------|-------|
| v0.7.5 | OTA size and version checks, PORTAL_KEEP_AP_ACTIVE, PORTAL_REBOOT_ON_TIMEOUT |
| v0.7.0 | OTA browser upload with progress bar + rollback, 4MB partition table, PORTAL_MAX_RETRY in Kconfig |
| v0.6.0 | Board-portable GPIO long press, portal_run() loop, try_sta_connect() refactor, /exit flag pattern, WPA3/OWE guards |
| v0.5.0 | Split into main + handlers files, full info page, menu page, all routes wired |
| v0.4.x | Single-file portal, NVS credentials, STA connect with retry, UI redesign |
| v0.1–0.3 | SoftAP example base, NVS helpers, HTTP server, WiFi scan |

--- 

## License

This project is licensed under the **GNU General Public License v3.0**.

You are free to use, modify, and distribute this software, provided that any distributed modifications or derivative works are also released under GPL v3 with full source code. See the `LICENSE` file for the full license text or visit [gnu.org/licenses/gpl-3.0](https://www.gnu.org/licenses/gpl-3.0).

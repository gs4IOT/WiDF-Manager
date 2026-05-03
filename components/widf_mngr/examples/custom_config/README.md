# WIDF Manager — Custom Config Example

Demonstrates runtime configuration using `widf_mngr_config_t` and flow control modes.

## What it shows

- Custom AP SSID and mDNS hostname (`mydevice.local`)
- Custom GPIO pin and long press duration
- Shorter portal timeout (60 seconds)
- AP security — commented block showing WPA2/WPA3 configuration
- Portal authentication — commented block protecting `/ota` and `/erase`
- `fallback_mode` — behavior when no saved network connects on boot
- `on_save_mode` — behavior after credentials are saved via the portal
- `on_connect_mode` — behavior after STA connects successfully

## AP security

Uncomment the AP security block in `main.c` to enable WPA2/WPA3:

```c
strncpy(cfg.ap_password, "mypassword", sizeof(cfg.ap_password));
cfg.ap_authmode = WIFI_AUTH_WPA2_WPA3_PSK;
```

When using the Kconfig default SSID (`WIDF-MANAGER`) with a secured auth mode, the last 4 MAC hex chars are appended automatically — e.g. `WIDF-MANAGER-A348`. If the password is empty or too short, a MAC-derived fallback is used and `WIDF_EVENT_AP_PASSWORD_FALLBACK` is fired.

## Portal authentication

Uncomment the auth block in `main.c` to protect `/ota` and `/erase`:

```c
strncpy(cfg.auth_password, "adminpass", sizeof(cfg.auth_password));
```

Users will be redirected to `/auth` when accessing protected routes. Leave `auth_password` empty (default) to disable authentication entirely.

## Flow control modes

Each mode field has multiple options — see the comments in `main.c` for the full list. The defaults shown in this example are:

| Field | Value | Behavior |
|-------|-------|----------|
| `fallback_mode` | `WIDF_FALLBACK_AP_PORTAL` | Open AP and portal if no network connects |
| `on_save_mode` | `WIDF_ON_SAVE_RECONNECT` | Reconnect immediately after credentials saved, no reboot |
| `on_connect_mode` | `WIDF_ON_CONNECT_PORTAL_STA` | Portal stays up on STA IP after connect |

## Setup

```bash
idf.py set-target esp32c3   # change to your target
idf.py build
idf.py flash monitor
```

## GPIO pin by board

| Board | Button GPIO |
|-------|-------------|
| M5Stamp C3 | 3 (default) |
| ESP32 DevKit | 0 |
| ESP32-C6 DevKit | 9 |
| ESP32-S3 DevKit | 0 |

Once connected, the portal is reachable at `mydevice.local` or the device STA IP shown in the serial monitor.

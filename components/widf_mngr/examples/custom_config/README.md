# WIDF Manager — Custom Config Example

Demonstrates runtime configuration using widf_mngr_config_t.

## What it shows

- Custom AP SSID
- Custom mDNS hostname (mydevice.local)
- sta_access mode — portal on STA IP after connect
- Custom GPIO pin and long press duration
- Shorter portal timeout

## Setup

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

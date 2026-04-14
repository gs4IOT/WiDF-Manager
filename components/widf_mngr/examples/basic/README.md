# WIDF Manager — Basic Example

Minimal integration using all Kconfig defaults.

## Setup

```bash
idf.py set-target esp32c3
idf.py menuconfig    # WIDF Manager Configuration → set GPIO pin
idf.py build
idf.py flash monitor
```

## What it does

Calls `widf_mngr_run(NULL)` — the library handles everything.
Connect to the `WIDF-MANAGER` AP and navigate to `192.168.4.1`.

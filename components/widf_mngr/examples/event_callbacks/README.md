# WIDF Manager — Event Callbacks Example

Demonstrates how to receive lifecycle events from WIDF Manager using the
`on_event` callback field in `widf_mngr_config_t`.

## What it shows

- Registering an event callback via `cfg.on_event`
- Handling all seven lifecycle events with appropriate log levels
- Accessing event data fields (SSID, IP, retry count)
- Distinction between `STA_FAILED` (connect attempt exhausted) and `STA_DISCONNECTED` (mid-session drop)

## Events

| Event | When fired | Data available |
|-------|-----------|----------------|
| `WIDF_EVENT_STA_TRYING` | Before connect attempt, once per network | `ssid`, `retries` |
| `WIDF_EVENT_STA_CONNECTED` | STA got IP | `ssid`, `ip` |
| `WIDF_EVENT_STA_DISCONNECTED` | Mid-session drop | `ssid` |
| `WIDF_EVENT_STA_FAILED` | All retries exhausted | `ssid`, `retries` |
| `WIDF_EVENT_PORTAL_OPENED` | HTTP server started | — |
| `WIDF_EVENT_PORTAL_CLOSED` | HTTP server stopped | — |
| `WIDF_EVENT_CREDENTIALS_SAVED` | New credentials in NVS | `ssid` |

## Notes

- The callback runs in the `widf_mngr` task context — keep it non-blocking
- Set `cfg.on_event = NULL` to disable callbacks entirely
- All other config fields use Kconfig defaults in this example

## Setup

```bash
idf.py set-target esp32c3   # change to your target
idf.py build
idf.py flash monitor
```

Connect to the `WIDF-MANAGER` AP and open `192.168.4.1` in a browser.
All state transitions will appear in the serial monitor.

#include "widf_mngr.h"
#include "esp_log.h"

static const char *TAG = "app";

static void on_widf_event(const widf_event_data_t *evt)
{
    switch (evt->event) {
        case WIDF_EVENT_STA_TRYING:
            ESP_LOGI(TAG, "EVENT: STA_TRYING — %s (%d retries budget)",
                     evt->data.trying.ssid, evt->data.trying.retries);
            break;
        case WIDF_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "EVENT: STA_CONNECTED — %s @ %s",
                     evt->data.connected.ssid, evt->data.connected.ip);
            break;
        case WIDF_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "EVENT: STA_DISCONNECTED — %s",
                     evt->data.disconnected.ssid);
            break;
        case WIDF_EVENT_STA_FAILED:
            ESP_LOGE(TAG, "EVENT: STA_FAILED — %s after %d retries",
                     evt->data.failed.ssid, evt->data.failed.retries);
            break;
        case WIDF_EVENT_PORTAL_OPENED:
            ESP_LOGI(TAG, "EVENT: PORTAL_OPENED");
            break;
        case WIDF_EVENT_PORTAL_CLOSED:
            ESP_LOGI(TAG, "EVENT: PORTAL_CLOSED");
            break;
        case WIDF_EVENT_CREDENTIALS_SAVED:
            ESP_LOGI(TAG, "EVENT: CREDENTIALS_SAVED — %s",
                     evt->data.saved.ssid);
            break;
        case WIDF_EVENT_AP_PASSWORD_FALLBACK:
            ESP_LOGW(TAG, "AP password %s — fallback password: %s",
                     evt->data.ap_fallback.reason,
                     evt->data.ap_fallback.password);
            break;
    }
}

void app_main(void)
{
    widf_mngr_config_t cfg = WIDF_MNGR_DEFAULT_CONFIG();
    cfg.on_event = on_widf_event;
    widf_mngr_run(&cfg);
}

/*  widf_mngr_handlers.h — HTTP handler declarations for WIDF Manager
 *    Include this in widf_mngr_main.c to access start_webserver(),
 *    the scan options buffer, and the shared exit/server globals.
 */

#ifndef WIDF_MNGR_HANDLERS_H
#define WIDF_MNGR_HANDLERS_H

#include "esp_http_server.h"
#include "esp_err.h"

/* ── Shared page CSS macro ───────────────────────────────────────────────── */
/* Generates the HTML head + shared styles for all simple inline pages.
 *   Use %% inside snprintf strings that follow this macro (escapes the % sign).
 *   The /info and /wifi pages use their own inline CSS for layout flexibility. */
#define PAGE_HEAD(title) \
"<!DOCTYPE html><html><head>" \
"<meta charset='utf-8'>" \
"<meta name='viewport' content='width=device-width,initial-scale=1'>" \
"<title>" title "</title>" \
"<style>" \
"*{box-sizing:border-box;margin:0;padding:0}" \
"body{font-family:-apple-system,sans-serif;background:#f0f2f5;" \
"min-height:100vh;display:flex;align-items:center;" \
"justify-content:center;padding:16px}" \
".card{background:#fff;border-radius:16px;" \
"box-shadow:0 4px 24px rgba(0,0,0,.10);" \
"padding:28px 24px;width:100%%;max-width:360px;text-align:center}" \
"h2{font-size:1.2rem;font-weight:700;color:#1a1a2e;margin-bottom:8px}" \
"p{color:#888;font-size:.9rem;margin-bottom:16px;line-height:1.5}" \
".btn{display:block;width:100%%;padding:12px;border:none;" \
"border-radius:10px;font-size:.93rem;font-weight:600;" \
"cursor:pointer;margin-top:10px;text-align:center;text-decoration:none}" \
".pri{background:#4361ee;color:#fff}" \
".sec{background:#f0f2f5;color:#444}" \
".warn{background:#fff0f0;color:#c0392b}" \
"</style></head><body>"

/* ── Shared scan options buffer ──────────────────────────────────────────── */
/* Populated by wifi_scan() in widf_mngr_main.c, consumed by portal_get_handler. */
extern char g_scan_options[6144];

/* ── Shared server handle and exit flag ──────────────────────────────────── */
/* g_server      — set by start_webserver(), used by exit_handler to stop it.
 *   g_exit_requested — set by exit_handler after sending response; polled by
 *                      portal_run() in main to break out of the portal loop.
 *   Both are declared in widf_mngr_handlers.c. */
extern httpd_handle_t    g_server;
extern volatile bool     g_exit_requested;

/* ── Handler declarations ────────────────────────────────────────────────── */
esp_err_t menu_get_handler    (httpd_req_t *req);
esp_err_t portal_get_handler  (httpd_req_t *req);
esp_err_t wifi_refresh_handler(httpd_req_t *req);
esp_err_t save_post_handler   (httpd_req_t *req);
esp_err_t info_get_handler    (httpd_req_t *req);
esp_err_t erase_handler       (httpd_req_t *req);
esp_err_t ota_get_handler     (httpd_req_t *req);
esp_err_t ota_upload_handler  (httpd_req_t *req);
esp_err_t restart_handler     (httpd_req_t *req);
esp_err_t exit_handler        (httpd_req_t *req);

/* ── Web server ──────────────────────────────────────────────────────────── */
/* Starts the HTTP server, registers all 9 routes, stores handle in g_server. */
httpd_handle_t start_webserver(void);

#endif /* WIDF_MNGR_HANDLERS_H */

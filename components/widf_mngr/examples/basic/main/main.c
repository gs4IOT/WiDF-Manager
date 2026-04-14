/*  basic — WIDF Manager minimal integration example
 *
 *  Demonstrates single-call integration using all Kconfig defaults.
 *  Configure AP SSID, GPIO pin, and timeout via idf.py menuconfig.
 */

#include "widf_mngr.h"

void app_main(void)
{
    widf_mngr_run(NULL);
}

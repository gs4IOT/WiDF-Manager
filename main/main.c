#include "widf_mngr.h"

void app_main(void)
{
    widf_mngr_config_t cfg = WIDF_MNGR_DEFAULT_CONFIG();
    widf_mngr_run(&cfg);
}

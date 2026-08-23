
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
LOG_MODULE_REGISTER(nrf9160_feather_startup);

static int hfclk_setup(void)
{

    struct onoff_manager *clk_mgr;
    static struct onoff_client cli = {};

    clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    sys_notify_init_spinwait(&cli.notify);
    (void)onoff_request(clk_mgr, &cli);

    return 0;
}

SYS_INIT(hfclk_setup, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

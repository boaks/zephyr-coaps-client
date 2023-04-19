/*
 * Copyright (c) 2023 Achim Kraus CloudCoap.net
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0
 *
 * SPDX-License-Identifier: EPL-2.0
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery_adc.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define MAX_DITHER 2
#define MAX_LOOPS 15

#define MEASURE_INTERVAL_MILLIS 50

#define SAMPLE_MIN_INTERVAL_MILLIS 5000

#define VBATT DT_PATH(vbatt)

struct battery_adc_config {
   struct adc_channel_cfg adc_cfg;
   struct gpio_dt_spec power_gpios;
   const struct device *adc;
   uint32_t output_ohm;
   uint32_t full_ohm;
};

static const struct battery_adc_config battery_adc_config = {
    .adc = DEVICE_DT_GET(DT_IO_CHANNELS_CTLR(VBATT)),
#if DT_NODE_HAS_PROP(VBATT, power_gpios)
    .power_gpios = GPIO_DT_SPEC_GET(VBATT, power_gpios),
#endif
    .output_ohm = DT_PROP(VBATT, output_ohms),
    .full_ohm = DT_PROP(VBATT, full_ohms),
    .adc_cfg = {
        .channel_id = 0,
        .gain = ADC_GAIN_1_6,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
        .input_positive = DT_PROP(VBATT, output_ohms) ? SAADC_CH_PSELP_PSELP_AnalogInput0 + DT_IO_CHANNELS_INPUT(VBATT) : SAADC_CH_PSELP_PSELP_VDD,
    },
};

static int16_t adc_raw_data = 0;

static struct adc_sequence adc_seq = {
    .channels = BIT(0),
    .buffer = &adc_raw_data,
    .buffer_size = sizeof(adc_raw_data),
    .resolution = 12,
    .oversampling = 5,
    .calibrate = true,
};

static volatile bool battery_adc_ok = false;
static volatile uint16_t battery_adc_last_voltage = 0;
static volatile int64_t battery_adc_last_uptime = 0;

static int battery_adc_setup(const struct device *arg)
{
   const struct gpio_dt_spec *gcp = &battery_adc_config.power_gpios;
   int rc = -ENOTSUP;

   if (!device_is_ready(battery_adc_config.adc)) {
      LOG_ERR("ADC device is not ready %s", battery_adc_config.adc->name);
      return rc;
   }

   if (device_is_ready(gcp->port)) {
      rc = gpio_pin_configure_dt(gcp, GPIO_OUTPUT_INACTIVE);
      if (rc != 0) {
         LOG_ERR("Failed to control feed %s.%u: %d",
                 gcp->port->name, gcp->pin, rc);
         return rc;
      }
   }

   rc = adc_channel_setup(battery_adc_config.adc, &battery_adc_config.adc_cfg);
   if (rc) {
      LOG_ERR("Setup AIN%u failed %d", DT_IO_CHANNELS_INPUT(VBATT), rc);
   } else {
      battery_adc_ok = true;
      LOG_INF("Battery ADC setup OK.");
   }
   return rc;
}

SYS_INIT(battery_adc_setup, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int battery_adc(uint16_t *voltage)
{
   int rc = 0;

   adc_seq.calibrate = true;
   rc = adc_read(battery_adc_config.adc, &adc_seq);
   adc_seq.calibrate = false;
   if (!rc) {
      int loop = MAX_LOOPS;
      int32_t val = adc_raw_data;
      k_sleep(K_MSEC(MEASURE_INTERVAL_MILLIS));
      rc = adc_read(battery_adc_config.adc, &adc_seq);
      while (!rc && loop > 0 && abs(val - adc_raw_data) > MAX_DITHER) {
         --loop;
         val = adc_raw_data;
         k_sleep(K_MSEC(MEASURE_INTERVAL_MILLIS));
         rc = adc_read(battery_adc_config.adc, &adc_seq);
      }
      if (!rc) {
         val = (val + adc_raw_data) / 2;
         adc_raw_to_millivolts(adc_ref_internal(battery_adc_config.adc),
                               battery_adc_config.adc_cfg.gain,
                               adc_seq.resolution,
                               &val);

         if (battery_adc_config.output_ohm != 0) {
            val = val * (uint64_t)battery_adc_config.full_ohm / battery_adc_config.output_ohm;
         }
         LOG_INF("#%d raw %u => %u mV", MAX_LOOPS - loop, adc_raw_data, val);
         battery_adc_last_voltage = (uint16_t)val;
         battery_adc_last_uptime = k_uptime_get();
         if (voltage) {
            *voltage = battery_adc_last_voltage;
         }
      }
   }

   return rc;
}

int battery_measure_enable(bool enable)
{
   int rc = -ENOTSUP;

   if (battery_adc_ok) {
      const struct gpio_dt_spec *gcp = &battery_adc_config.power_gpios;

      if (device_is_ready(gcp->port)) {
         rc = gpio_pin_set_dt(gcp, enable);
      }
   }
   return rc;
}

int battery_sample(uint16_t *voltage)
{
   int rc = -ENOTSUP;

   if (battery_adc_ok) {
      int64_t now = k_uptime_get();
      if (voltage && battery_adc_last_uptime &&
          (now - battery_adc_last_uptime) < SAMPLE_MIN_INTERVAL_MILLIS) {
         rc = 0;
         *voltage = battery_adc_last_voltage;
         LOG_INF("last voltage %u mV", *voltage);
      } else {
         rc = battery_measure_enable(true);
         if (!rc) {
            k_sleep(K_MSEC(10));
            rc = battery_adc(voltage);
            battery_measure_enable(false);
         }
      }
   }

   return rc;
}

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
#define MIN_SAMPLES 4
#define MAX_LOOPS 15

#define MEASURE_INTERVAL_MILLIS 50
#define SAMPLE_MIN_INTERVAL_MILLIS 10000

#define VBATT DT_PATH(vbatt)

struct battery_adc_config {
   const char *name;
   struct adc_channel_cfg adc_cfg;
   struct gpio_dt_spec power_gpios;
   const struct device *adc;
   uint8_t adc_channel;
   uint32_t output_ohm;
   uint32_t full_ohm;
   uint32_t sample_min_interval;
};

struct battery_adc_status {
   bool ok;
   uint16_t last_voltage;
   int64_t last_uptime;
};

#define CREATE_VBATT_INSTANCE(NODE, ID, INTERVAL)                                                                                                             \
   static const struct battery_adc_config battery_adc_config_##NODE = {                                                                                       \
       .name = #NODE,                                                                                                                                         \
       .adc = DEVICE_DT_GET(DT_IO_CHANNELS_CTLR(NODE)),                                                                                                       \
       .adc_channel = DT_IO_CHANNELS_INPUT(NODE),                                                                                                             \
       .power_gpios = GPIO_DT_SPEC_GET_OR(NODE, power_gpios, {}),                                                                                             \
       .output_ohm = DT_PROP(NODE, output_ohms),                                                                                                              \
       .full_ohm = DT_PROP(NODE, full_ohms),                                                                                                                  \
       .sample_min_interval = INTERVAL,                                                                                                                       \
       .adc_cfg = {                                                                                                                                           \
           .channel_id = ID,                                                                                                                                  \
           .gain = ADC_GAIN_1_6,                                                                                                                              \
           .reference = ADC_REF_INTERNAL,                                                                                                                     \
           .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),                                                                                   \
           .input_positive = DT_NODE_HAS_PROP(NODE, output_ohms) ? SAADC_CH_PSELP_PSELP_AnalogInput0 + DT_IO_CHANNELS_INPUT(NODE) : SAADC_CH_PSELP_PSELP_VDD, \
       },                                                                                                                                                     \
   }


CREATE_VBATT_INSTANCE(VBATT, 0, SAMPLE_MIN_INTERVAL_MILLIS);
static volatile struct battery_adc_status battery_status_VBATT = {false, 0, 0};

static int16_t adc_raw_data = 0;

static struct adc_sequence adc_seq = {
    .channels = BIT(0),
    .buffer = &adc_raw_data,
    .buffer_size = sizeof(adc_raw_data),
    .resolution = 12,
    .oversampling = 5,
    .calibrate = true,
};

static int battery_adc_setup_inst(const struct battery_adc_config *cfg, volatile struct battery_adc_status *status)
{
   const struct gpio_dt_spec *gcp = &cfg->power_gpios;
   int rc = -ENOTSUP;

   if (!device_is_ready(cfg->adc)) {
      if (cfg->adc) {
         LOG_ERR("ADC device is not ready %s", cfg->adc->name);
      } else {
         LOG_ERR("ADC device is not ready %s", cfg->name);
      }
      return rc;
   }

   if (device_is_ready(gcp->port)) {
      rc = gpio_pin_configure_dt(gcp, GPIO_OUTPUT_INACTIVE);
      if (rc != 0) {
         LOG_ERR("Failed to control feed %s.%u: %d",
                 gcp->port->name, gcp->pin, rc);
         return rc;
      }
      LOG_INF("%s %s:%u configured.", cfg->name, gcp->port->name, gcp->pin);
   }

   rc = adc_channel_setup(cfg->adc, &cfg->adc_cfg);
   if (rc) {
      LOG_ERR("Setup AIN%u failed %d", cfg->adc_channel, rc);
   } else {
      status->ok = true;
      LOG_INF("Battery %s ADC channel %u setup OK.", cfg->name, cfg->adc_channel);
   }
   return rc;
}

static int battery_adc_setup(void)
{
   return battery_adc_setup_inst(&battery_adc_config_VBATT, &battery_status_VBATT);
}

SYS_INIT(battery_adc_setup, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int battery_adc_inst(const struct battery_adc_config *cfg,
                            volatile struct battery_adc_status *status, uint16_t *voltage)
{
   int rc = 0;

   adc_seq.calibrate = true;
   adc_seq.channels = BIT(cfg->adc_cfg.channel_id);
   rc = adc_read(cfg->adc, &adc_seq);
   adc_seq.calibrate = false;
   if (!rc) {
      int loop = MAX_LOOPS;
      int counter = 0;
      int32_t values[MIN_SAMPLES];
      int32_t max = adc_raw_data;
      int32_t min = adc_raw_data;

      values[counter++] = adc_raw_data;
      k_sleep(K_MSEC(MEASURE_INTERVAL_MILLIS));
      rc = adc_read(cfg->adc, &adc_seq);
      while (!rc && loop > 0 && counter < MIN_SAMPLES) {
         --loop;
         if (min > adc_raw_data) {
            min = adc_raw_data;
         } else if (max < adc_raw_data) {
            max = adc_raw_data;
         }
         if (max - min > MAX_DITHER) {
            counter = 1;
            values[0] = adc_raw_data;
            if (loop > 0) {
               max = adc_raw_data;
               min = adc_raw_data;
            }
         } else {
            values[counter++] = adc_raw_data;
         }
         k_sleep(K_MSEC(MEASURE_INTERVAL_MILLIS));
         rc = adc_read(cfg->adc, &adc_seq);
      }
      if (!rc && counter == MIN_SAMPLES) {
         int32_t val_avg = values[0];
         for (counter = 1; counter < MIN_SAMPLES; ++counter) {
            val_avg += values[counter];
         }
         val_avg = (val_avg + (counter / 2)) / counter;
         adc_raw_data = val_avg;
         adc_raw_to_millivolts(adc_ref_internal(cfg->adc),
                               cfg->adc_cfg.gain,
                               adc_seq.resolution,
                               &val_avg);
         if (cfg->output_ohm != 0) {
            val_avg = val_avg * (uint64_t)cfg->full_ohm / cfg->output_ohm;
         }
         LOG_INF("#%s %d raw %u => %u mV", cfg->name, MAX_LOOPS - loop, adc_raw_data, val_avg);
         status->last_voltage = (uint16_t)val_avg;
         status->last_uptime = k_uptime_get();
         if (voltage) {
            *voltage = (uint16_t)val_avg;
         }
      }
   }

   return rc;
}

static int battery_measure_enable_inst(const struct battery_adc_config *cfg,
                                       volatile struct battery_adc_status *status, bool enable)
{
   int rc = -ENOTSUP;

   if (status->ok) {
      const struct gpio_dt_spec *gcp = &cfg->power_gpios;

      if (device_is_ready(gcp->port)) {
         rc = gpio_pin_set_dt(gcp, enable);
      }
   }

   return rc;
}

static int battery_sample_inst(const struct battery_adc_config *cfg,
                               volatile struct battery_adc_status *status, uint16_t *voltage)
{
   int rc = -ENOTSUP;

   if (status->ok) {
      int64_t now = k_uptime_get();
      if (voltage && status->last_uptime &&
          (now - status->last_uptime) < cfg->sample_min_interval) {
         rc = 0;
         *voltage = status->last_voltage;
         LOG_INF("%s last voltage %u mV", cfg->name, *voltage);
      } else {
         rc = battery_measure_enable_inst(cfg, status, true);
         if (!rc) {
            k_sleep(K_MSEC(10));
            rc = battery_adc_inst(cfg, status, voltage);
            battery_measure_enable_inst(cfg, status, false);
         }
      }
   }

   return rc;
}

int battery_measure_enable(bool enable)
{
   return battery_measure_enable_inst(&battery_adc_config_VBATT, &battery_status_VBATT, enable);
}

int battery_sample(uint16_t *voltage)
{
   return battery_sample_inst(&battery_adc_config_VBATT, &battery_status_VBATT, voltage);
}

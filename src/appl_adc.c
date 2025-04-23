/*
 * Copyright (c) 2025 Achim Kraus CloudCoap.net
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

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "sh_cmd.h"

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || \
    !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define DT_SPEC_AND_COMMA(node_id, prop, idx) \
   ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

/* Data of ADC io-channels specified in devicetree. */
static const struct adc_dt_spec adc_channels[] = {
    DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels,
                         DT_SPEC_AND_COMMA)};

int appl_adc_sample(int channel, int max_sample, int max_dither, uint16_t *voltage)
{
   int err = 0;
   int res = -EINVAL;
   uint16_t buf = 0;
   struct adc_sequence sequence = {
       .buffer = &buf,
       /* buffer size in bytes, not number of samples */
       .buffer_size = sizeof(buf),
   };
   struct adc_dt_spec adc_channel;

   if (channel < 0 || ARRAY_SIZE(adc_channels) <= channel) {
      LOG_WRN("ADC channel %d not available", channel);
      return -ENOTSUP;
   }
   adc_channel = adc_channels[channel];
#ifdef CONFIG_ADC_SENSOR_CHANNEL_0_MUX
   // overwrite logic channel id by 0
   adc_channel.channel_id = 0;
   adc_channel.channel_cfg.channel_id = 0;
#endif

   LOG_INF(" - %s, channel %d: ", adc_channel.dev->name, channel);

   if (!adc_is_ready_dt(&adc_channel)) {
      LOG_INF("ADC controller device %s not ready\n", adc_channel.dev->name);
      return -EINVAL;
   }

   err = adc_channel_setup_dt(&adc_channel);
   if (err < 0) {
      LOG_INF("Could not setup channel %d (%d, %s)\n", channel, err, strerror(-err));
      return -EINVAL;
   }

   for (int k = 0; k < max_sample; k++) {
      LOG_INF("ADC reading[%u]:\n", k);
      int32_t val_mv;

      (void)adc_sequence_init_dt(&adc_channel, &sequence);

      err = adc_read_dt(&adc_channel, &sequence);
      if (err < 0) {
         LOG_INF("Could not read channel %d (%d, %s)\n", channel, err, strerror(-err));
         continue;
      }

      /*
       * If using differential mode, the 16 bit value
       * in the ADC sample buffer should be a signed 2's
       * complement value.
       */
      if (adc_channel.channel_cfg.differential) {
         val_mv = (int32_t)((int16_t)buf);
      } else {
         val_mv = (int32_t)buf;
      }
      LOG_INF("%" PRId32, val_mv);
      err = adc_raw_to_millivolts_dt(&adc_channel,
                                     &val_mv);
      /* conversion to mV may not be supported, skip if not */
      if (err < 0) {
         LOG_INF(" (value in mV not available)\n");
      } else {
         LOG_INF(" = %" PRId32 " mV\n", val_mv);
      }
      if (voltage) {
         *voltage = val_mv;
      }
      res = 0;
   }
   return res;
}

int appl_adc_sample_desc(char *buf, size_t len)
{
   int err = 0;
   int index = 0;
   uint16_t voltage = 0;

   for (uint16_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
      err = appl_adc_sample(i, 10, 5, &voltage);
      if (!err) {
         index += snprintf(buf + index, len - index, "CH %u, %u mV ", i, voltage);
      }
   }
   return index;
}

#ifdef CONFIG_SH_CMD

static char cmd_buf[64];

static int sh_cmd_adc(const char *parameter)
{
   (void)parameter;
   return appl_adc_sample_desc(cmd_buf, sizeof(cmd_buf));
}

SH_CMD(adc, NULL, "read ADC.", sh_cmd_adc, NULL, 0);
#endif /* CONFIG_SH_CMD */

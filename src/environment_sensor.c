/*
 * Copyright (c) 2022 Achim Kraus CloudCoap.net
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

#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "environment_sensor.h"

#if defined(CONFIG_BME680_BSEC) || defined(CONFIG_BME680) || defined(CONFIG_SHT3XD)

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static const float temperature_offset = (CONFIG_TEMPERATURE_OFFSET / 100.0);

#ifdef CONFIG_BME680_BSEC

#include "bsec_integration.h"
#include "bsec_serialized_configurations_iaq.h"
#include <zephyr/drivers/i2c.h>

/*
 * BSEC_SAMPLE_RATE_ULP = 0.0033333 Hz = 300 second interval
 * BSEC_SAMPLE_RATE_LP = 0.33333 Hz = 3 second interval
 */
#if defined(CONFIG_BME680_BSEC_SAMPLE_MODE_ULTRA_LOW_POWER)
#define BSEC_SAMPLE_RATE BSEC_SAMPLE_RATE_ULP
#elif defined(CONFIG_BME680_BSEC_SAMPLE_MODE_LOW_POWER)
#define BSEC_SAMPLE_RATE BSEC_SAMPLE_RATE_LP
#endif

static K_MUTEX_DEFINE(environment_mutex);

K_THREAD_STACK_DEFINE(thread_stack, CONFIG_BME680_BSEC_THREAD_STACK_SIZE);

static struct k_thread environment_thread;
static const struct device *environment_i2c = DEVICE_DT_GET(DT_BUS(DT_ALIAS(environment_sensor)));

/* Structure used to maintain internal variables used by the library. */
static struct environment_values {

   float temperature;
   float humidity;
   float pressure;
   float gas;
   float air_quality;

} environment_values;

static int8_t environment_bus_write(uint8_t dev_addr, uint8_t reg_addr, uint8_t *reg_data_ptr, uint16_t len)
{
   uint8_t buf[len + 1];

   buf[0] = reg_addr;
   memcpy(&buf[1], reg_data_ptr, len);

   return i2c_write(environment_i2c, buf, len + 1, dev_addr);
}

static int8_t environment_bus_read(uint8_t dev_addr, uint8_t reg_addr, uint8_t *reg_data_ptr, uint16_t len)
{
   return i2c_write_read(environment_i2c, dev_addr, &reg_addr, 1, reg_data_ptr, len);
}

static int64_t environment_get_timestamp_us(void)
{
   return k_uptime_get() * USEC_PER_MSEC;
}

static void environment_delay_ms(uint32_t period)
{
   k_sleep(K_MSEC(period));
}

static void environment_output_ready(int64_t timestamp, float iaq, uint8_t iaq_accuracy, float temperature,
                                     float humidity, float pressure, float raw_temperature, float raw_humidity,
                                     float gas, bsec_library_return_t bsec_status, float static_iaq,
                                     float co2_equivalent, float breath_voc_equivalent)
{
   double temp;
   k_mutex_lock(&environment_mutex, K_FOREVER);

   environment_values.temperature = temperature - temperature_offset; /* compensate self heating */
   environment_values.humidity = humidity;
   environment_values.pressure = pressure / 100.0; /* hPA */
   environment_values.gas = gas;
   environment_values.air_quality = iaq;
   temp = environment_values.temperature;
   k_mutex_unlock(&environment_mutex);

   environment_add_temperature_history(temp, false);
}

static uint32_t environment_state_load(uint8_t *state_buffer, uint32_t n_buffer)
{
   return 0;
}

static void environment_state_save(const uint8_t *state_buffer, uint32_t length)
{
}

static uint32_t environment_config_load(uint8_t *config_buffer, uint32_t n_buffer)
{
   if (sizeof(bsec_config_iaq) <= n_buffer) {
      memcpy(config_buffer, bsec_config_iaq, sizeof(bsec_config_iaq));
      return sizeof(bsec_config_iaq);
   }
   return 0;
}

static void environment_bsec_thread_fn(void)
{
   bsec_iot_loop(environment_delay_ms, environment_get_timestamp_us, environment_output_ready, environment_state_save, 0xffffffff);
}

int environment_init(void)
{
   return_values_init bsec_ret;

   LOG_INF("BME680 BSEC initialize, %f Hz", BSEC_SAMPLE_RATE);
   if (!device_is_ready(environment_i2c)) {
      LOG_ERR("%s device is not ready", environment_i2c->name);
      return -ENOTSUP;
   }
   environment_init_temperature_history();

   bsec_ret = bsec_iot_init(BSEC_SAMPLE_RATE, temperature_offset, environment_bus_write, environment_bus_read, environment_delay_ms,
                            environment_state_load, environment_config_load);

   if (bsec_ret.bme680_status) {
      LOG_ERR("Could not initialize BME680: %d", bsec_ret.bme680_status);
      return -EIO;
   } else if (bsec_ret.bsec_status) {
      LOG_ERR("Could not initialize BSEC library: %d", bsec_ret.bsec_status);
      return -EIO;
   }

   k_thread_create(&environment_thread,
                   thread_stack,
                   CONFIG_BME680_BSEC_THREAD_STACK_SIZE,
                   (k_thread_entry_t)environment_bsec_thread_fn,
                   NULL, NULL, NULL, K_HIGHEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);

   return 0;
}

int environment_sensor_fetch(bool force)
{
   (void)force;
   return 0;
}

int environment_get_float(double *value, const float *current)
{
   k_mutex_lock(&environment_mutex, K_FOREVER);
   *value = *current;
   k_mutex_unlock(&environment_mutex);
   return 0;
}

int environment_get_int32(int32_t *value, const float *current)
{
   k_mutex_lock(&environment_mutex, K_FOREVER);
   *value = *current;
   k_mutex_unlock(&environment_mutex);
   return 0;
}

int environment_get_temperature(double *value)
{
   return environment_get_float(value, &environment_values.temperature);
}

int environment_get_humidity(double *value)
{
   return environment_get_float(value, &environment_values.humidity);
}

int environment_get_pressure(double *value)
{
   return environment_get_float(value, &environment_values.pressure);
}

int environment_get_gas(int32_t *value)
{
   return environment_get_int32(value, &environment_values.gas);
}

int environment_get_iaq(int32_t *value)
{
   return environment_get_int32(value, &environment_values.air_quality);
}

#else /* CONFIG_BME680_BSEC */

struct environment_sensor {
   const enum sensor_channel channel;
   const struct device *dev;
};

static const struct environment_sensor temperature_sensor = {
    .channel = SENSOR_CHAN_AMBIENT_TEMP,
    .dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(temperature_sensor))};

static const struct environment_sensor humidity_sensor = {
    .channel = SENSOR_CHAN_HUMIDITY,
    .dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(humidity_sensor))};

static const struct environment_sensor pressure_sensor = {
    .channel = SENSOR_CHAN_PRESS,
    .dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(pressure_sensor))};

static const struct environment_sensor gas_sensor = {
    .channel = SENSOR_CHAN_GAS_RES,
    .dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(gas_sensor))};

static const struct environment_sensor *all_sensors[] = {
    &temperature_sensor,
    &humidity_sensor,
    &pressure_sensor,
    &gas_sensor};

static const unsigned int all_sensors_size = sizeof(all_sensors) / sizeof(void *);

int environment_sensor_fetch(bool force)
{
   static int64_t environment_sensor_next_fetch = 0;
   static int err = 0;
   int64_t now = k_uptime_get();
   if (force || (now - environment_sensor_next_fetch) >= 0) {
      environment_sensor_next_fetch = now + CONFIG_SAMPLE_INTERVAL_S * MSEC_PER_SEC;
      for (int index1 = 0; index1 < all_sensors_size; ++index1) {
         const struct device *dev = all_sensors[index1]->dev;
         if (dev) {
            for (int index2 = 0; index2 < index1; ++index2) {
               if (dev == all_sensors[index2]->dev) {
                  dev = NULL;
                  break;
               }
            }
            if (dev) {
               err = sensor_sample_fetch_chan(dev, SENSOR_CHAN_ALL);
               if (err) {
                  break;
               }
            }
         }
      }
   }
   return err;
}

static int environment_sensor_init(const struct device *dev)
{
   if (dev && !device_is_ready(dev)) {
      LOG_ERR("%s device is not ready", dev->name);
      return -ENOTSUP;
   }
   return 0;
}

int environment_init(void)
{
   int err = 0;
#ifdef CONFIG_BME680
   LOG_INF("BME680 initialize, %ds minimum interval", CONFIG_SAMPLE_INTERVAL_S);
#else
   LOG_INF("SHT3x initialize, %ds minimum interval", CONFIG_SAMPLE_INTERVAL_S);
#endif

   for (int index = 0; index < all_sensors_size; ++index) {
      err = environment_sensor_init(all_sensors[index]->dev);
      if (err) {
         return err;
      }
   }
   environment_sensor_fetch(true);
   environment_init_temperature_history();

   return 0;
}

static int environment_sensor_read(const struct environment_sensor *sensor, double *value, int32_t *high_value, int32_t *low_value)
{
   int err;
   struct sensor_value data = {0};

   if (!sensor->dev) {
      return -ENODATA;
   }

   err = environment_sensor_fetch(false);
   if (err) {
      LOG_ERR("Failed to fetch data from %s, error: %d",
              sensor->dev->name, err);
      return -ENODATA;
   }

   err = sensor_channel_get(sensor->dev, sensor->channel, &data);
   if (err) {
      LOG_ERR("Failed to read data from %s, error: %d",
              sensor->dev->name, err);
      return -ENODATA;
   }

   if (value) {
      *value = sensor_value_to_double(&data);
   } else {
      if (high_value) {
         *high_value = data.val1;
      }
      if (low_value) {
         *low_value = data.val2;
      }
   }
   return 0;
}

int environment_get_temperature(double *value)
{
   int err = environment_sensor_read(&temperature_sensor, value, NULL, NULL);
   if (!err) {
      *value -= temperature_offset; /* compensate self heating */
   }
   return err;
}

int environment_get_humidity(double *value)
{
   return environment_sensor_read(&humidity_sensor, value, NULL, NULL);
}

int environment_get_pressure(double *value)
{
   int err = environment_sensor_read(&pressure_sensor, value, NULL, NULL);
   if (!err) {
      *value *= 10.0; /* hPA */
   }
   return err;
}

int environment_get_gas(int32_t *value)
{
   return environment_sensor_read(&gas_sensor, NULL, value, NULL);
}

int environment_get_iaq(int32_t *value)
{
   return -ENODATA;
}

#endif /* CONFIG_BME680_BSEC */
#endif /* CONFIG_BME680_BSEC || CONFIG_BME680 || CONFIG_SHT3XD */

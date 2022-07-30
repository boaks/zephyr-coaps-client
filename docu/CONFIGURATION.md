![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

# Configuration

## Basic

- **COAP_SERVER_HOSTNAME**, hostname of the coap/dtls 1.2 cid server. Default is the Californium's sandbox at `californium.eclipseprojects.io`.

- **COAP_SERVER_ADDRESS_STATIC**, static ip-address of the coap/dtls 1.2 cid server. Fallback, if DNS isn't setup.

- **COAP_SERVER_PORT**, static ip-address of the coap/dtls 1.2 cid server. Fallback, if DNS isn't setup. Default `5684`.

- **DTLS_PSK_IDENTITY**, the PSK identiy. Default `cali.${imei}`, e.g. "cali.352656100985434".

- **DTLS_PSK_SECRET**, the PSK secret (ASCII). Default `.fornium`. The sandbox uses this secret for the a wildcard identity "cali.*". This is only intended to easy access the sandbox, don't use it on your own test setup.

- **COAP_SEND_INTERVAL**, coap send interval in seconds. Used, if messages are send frequently. Default 0s, disabled.

## Extensions

### GPS/GNSS (experimental)

- **LOCATION_ENABLE**, enable the GPS/GNSS support. Still experimental. Requires a lot of work. Default disabled.

- **LOCATION_ENABLE_CONTINUES_MODE**, enable to continously receive GPS/GNSS signals. With that the Thingy:91 receives the best positions but also requires the most energy (50mA). Default enabled.

- **LOCATION_ENABLE_TRIGGER_MESSAGE**, send a message, when a position is reported by GPS/GNSS. The not continues mode is still experimental. It the most cases, it stops working after something as 30 minutes and requires also 30 minutes to wokr again. So it still requires a lot of work. Default disabled.

### Sensors

- **ADP536X_POWER_MANAGEMENT**, enable Thingy's battery power management. Reports battery status (e.g. charging) and level (e.g. 70%). Default enabled.

- **ADXL362_MOTION_DETECTION**, use ADXL362 to detect, if the Thingy:91 is moved. Only rudimentary function. Default disabled.

- **ADXL362_MOTION_DETECTION_LED**, use green LED to signal detected move. Default disabled.

For the usage of the environment sensor [BME680 (Bosch)](https://www.bosch-sensortec.com/products/environmental-sensors/gas-sensors/bme680/) two options are available:

- **zephyr bme680 driver**, access to temperature, humidity, pressure, and the "gas sensor resistance". No additional source are required. Enabled at
`Zephyr Kernel > Device Drivers > Sensor Drivers > BME680 sensor`.

- **Bosch bme680 BSEC library**, access to temperature, humidity, pressure, and the IAQ (Index Air Quality).
The [BSEC](https://www.bosch-sensortec.com/software-tools/software/bsec/) must be downloaded from Bosch and comes with its own [license](https://www.bosch-sensortec.com/media/boschsensortec/downloads/bsec/2017-07-17_clickthrough_license_terms_environmentalib_sw_clean.pdf). Unzip the downloaded archive into the `zephyr-coaps-client/nrf/ext/` (or `<ncs>/nrf/ext/`). The resulting path must be `nrf/ext/BSEC_1.4.8.0_Generic_Release_updated_v3`.
If the preparation is done, disable 
`Zephyr Kernel > Device Drivers > Sensor Drivers > BME680 sensor` (if enabled),
and then enable
`Extra Functions > BME680 BSEC`.
instead.

**NOTE:** the BME680 is mounted inside the Thingy:91 and is therefore only weakly coupled to the environment. On battery charging, the temperature gets up to 30°. If you want more realisitc values, please remove the board from the case.

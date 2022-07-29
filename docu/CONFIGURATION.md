![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

** !!! Under Construction !!! **

# Configuration



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

** !!! Under Construction !!! **

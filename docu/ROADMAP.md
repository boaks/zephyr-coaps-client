![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

# Roadmap - Device

## History

| Date         | Version | Description |
| ------------ | ------- | ----------- |
| 01-June-2022 |  v0.1   | Initial version.<br/>Known issues:<br/>Does not reconnect after network lost.<br/>Does not start proper without network |
| 26-June-2022 |  v0.2   | Test experience.<br/>Both network issues are solved. |
| 01-August-2022 |  v0.4   | Enable sensors.<br/> Includes support for Bosch Sensortec library for BME680 |

(The 0.3 has been skip in order to not mix up to different build alreay used the 0.3.)

## Future

Ongoing developments:

- enable GNSS, reduce power consumption. To start GPS, it is usually required to place the device with free sight to the sky. AGPS and PGPS are for now not considered. The "location service" is even not planed to be used at all. Currently GPS/GNSS is disabled by default, maybe enabled using KConfig.

- enable accelerometer-meter. First promising results in order to enable GNSS using the "motion detection". Requires much more work to offer it as useful feature.

| Planed Date    | Version | Description |
| -------------- | ------- | ----------- |
| ??-September-2022 |   v?.?  | Improve accelerometer-meter |
| ??-November-2022 |   v?.?  | GNSS |

# Roadmap - Server

** !!! Under Construction !!! **

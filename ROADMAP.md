![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

# Roadmap - Device

## History

| Date         | Version | Description |
| ------------ | ------- | ----------- |
| 01-June-2022 |  v0.1   | Initial version.<br/>Known issues:<br/>Does not reconnect after network lost.<br/>Does not start proper without network |
| 26-June-2022 |  v0.2   | Test experience.<br/>Both network issues are solved. |

## Future

Ongoing developments:

- enable GNSS, reducs power consumption. Especially the `visibility_detection` seems to be tricky. The GPS signal level seems to be weak. Usually it's required to place the device with free sight to the sky, at least to start the GPS. AGPS and PGPS are for now not considered. The "location service" is even not planed to be used at all. Currently disabled by default, maybe enabled using KConfig.

- enable accelerometer-meter. First promising results in order to enable GNSS using the "motion detection". Requires much more work to offer it as useful feature. (Only private experimental code for now.) 

- use Bosch Sensortech library for BME680. Get better (calibrated) values, enable "air quality" and "air pressure".


| Planed Date    | Version | Description |
| -------------- | ------- | ----------- |
| 01-August-2022 |   v?.?  | use Bosch Sensortech library for BME680 |
| ??-August-2022 |   v?.?  | enable accelerometer-meter |
| ??-September-2022 |   v?.?  | GNSS |

# Roadmap - Server

** !!! Under Construction !!! **

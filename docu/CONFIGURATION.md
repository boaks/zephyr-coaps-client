![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

# Configuration

The demo client comes with some configuration values, which are setup using:

```
west build -b thingy91_nrf9160_ns -t menuconfig
```

for the console variant, and

```
west build -b thingy91_nrf9160_ns -t guiconfig
```

for the GUI variant. That enables to select individual features.

Alternatively you may use the project's configuration file [prj.conf](../prj.conf) to setup these features. If you want/need to uses several configuration variants, "overlays" work best. Such an overlay overwrittes the values of the standard project configuration. e.g.

[10min-prj.conf](../10min-prj.conf) prepares to send a message every 10 minutes. Enables to use a environment history to store sensor data.

For more information, see [overlays](#overlays).

## General

The time running from battery depends a lot from the configuration.

The application enables per default to suspend the UART and the 3.3V.

Depending on the SIM-Card, the HPPLMN (automatic search for higher priorized PLMN) consumes quite a lot energy. e.g. a HPPLMN interval of 2h and an average searchtime of 60s ends up at a power consumption comparable with 10 message exchanges every hour. So check your SIM-card (enable COAP_SEND_SIM_INFO), not that you get surprised!

The usage of the sensors also reduces that time. Consider to measure less frequent, e.g. every 10 minutes instead of every couple of seconds.

## Basic

- **COAP_SERVER_HOSTNAME**, hostname of the coap/dtls 1.2 cid server. Default is the Californium's sandbox at `californium.eclipseprojects.io`.

- **COAP_SERVER_ADDRESS_STATIC**, static ip-address of the coap/dtls 1.2 cid server. Fallback, if DNS isn't setup.

- **COAP_SERVER_PORT**, static ip-address of the coap/dtls 1.2 cid server. Fallback, if DNS isn't setup. Default `5684`.

- **DTLS_PSK_IDENTITY**, the PSK identiy. Default `cali.${imei}`, e.g. "cali.352656100985434".

- **DTLS_PSK_SECRET**, the PSK secret (ASCII). Default `.fornium`. The sandbox uses this secret for the a wildcard identity "cali.*". This is only intended to easy access the sandbox, don't use it on your own test setup.

- **COAP_SEND_INTERVAL**, coap send interval in seconds. Used, if messages are send frequently. Default 0s, disabled.

- **COAP_FAILURE_SEND_INTERVAL**, coap send interval after failures in seconds. 0 to use send interval.

- **COAP_WAIT_ON_POWERMANAGER**, coap waits for the power-manager to start exchanging application data. Takes up to 30 s and delays the first exchange. Default disabled.

- **COAP_SEND_SIM_INFO**, include information from the SIM-card.

```
ICCID: ????????????????????, eDRX cycle: off, HPPLMN interval: 10 [h]
IMSI: ???????????????
```

- **COAP_SEND_NETWORK_INFO**, include network information

```
!Network: CAT-M1,roaming,Band 3,#PLMN 26201,TAC 26553,Cell 30157574
PDN: ???.???,???.??.??.???
!PSM: TAU 86400 [s], Act 0 [s], Released: 12406 ms
```

- **COAP_SEND_STATISTIC_INFO**, include statistic information. e.g. transmissions and network searchs.

```
!CE: down: 8, up: 1, RSRP: -116 dBm, CINR: 8 dB, SNR: 9 dB
Stat: tx 1 kB, rx 0 kB, max 597 B, avg 142 B
Cell updates 2, Network searchs 1 (2 [s]), PSM delays 0 (0 [s]), Restarts 0
```

- **COAP_NO_RESPONSE_ENABLE**, send one-way coap message (request without response).

- **COAP_RESOURCE**, resource name of request. Default "echo".

## URI query parameter

Configure used URI query parameter. Please note, that these query parameter must be supported by the server's resource implementation. 

- **COAP_QUERY_DELAY_ENABLE**, request the server to respond delayed (use URI query parameter "delay=%d"). Only for NAT testing. Must also be enabled on server side.

- **COAP_QUERY_RESPONSE_LENGTH**, request the server to respond with a specific payload length (use URI query parameter "rlen=%d"). Large values may require blockwise transfer, which is not supported.

- **COAP_QUERY_KEEP_ENABLE**, request the server to keep the request (use URI query parameter "keep).

- **COAP_QUERY_ACK_ENABLE**, request the server to use a separate empty ACK (use URI query parameter "ack).

- **COAP_QUERY_SERIES_ENABLE**, request the server to append some of the data to a "series" (use URI query parameter "series). Currently only supported for text-payload. Lines starting with "!" are appended to the resource "series".

- **COAP_QUERY_READ_SUBRESOURCE_ENABLE**, request the server to read a sub-resource (use URI query parameter "read").

- **COAP_QUERY_READ_SUBRESOURCE**, name of the sub-resource. Default "config".

- **COAP_QUERY_WRITE_SUBRESOURCE_ENABLE**, request the server to write to a sub-resource (use URI query parameter "write").

- **COAP_QUERY_WRITE_SUBRESOURCE**, name of the sub-resource. Default "${now}".

## Extensions

### Modem

- **LTE_LOCK_PLMN_CONFIG_SWITCH**, use switch1 and switch2 of the nRf9160-DK to select the PLMN on start. on := GND, off := n.c.

- **LTE_LOCK_PLMN_CONFIG_SWITCH_STRING_1**, PLMN for switch1 on and switch2 off.

- **LTE_LOCK_PLMN_CONFIG_SWITCH_STRING_2**, PLMN for switch1 off and switch2 on.

- **LTE_LOCK_PLMN_CONFIG_SWITCH_STRING_3**, PLMN for switch1 on and switch2 on.

- **LTE_POWER_ON_OFF_ENABLE**, switch the modem on/off (AT+CFUN=0,AT+CFUN=x), when not in use. 

- **LTE_POWER_ON_OFF_CONFIG_SWITCH**, use switch1 to select PSM or modem on/off.

- **UDP_PSM_ENABLE**, enable PSM.

- **UDP_RAI_ENABLE**, enable RAI (control plane).

- **UDP_AS_RAI_ENABLE**, enable RAI (access stratum), requires 3GPP release 14 supported by the Mobile Network Operator. If not supported, the modem implicitly uses CP-RAI.

- **UDP_EDRX_ENABLE**, enable eDRX.

- **STATIONARY_MODE_ENABLE**, enable stationary mode. Optimize power consumption, if device is sationary at one place and is not moved.

- **MODEM_SAVE_CONFIG_THRESHOLD**, threshold to save the modem configuration. The modem is switched off and on to save the configuration.

- **MODEM_MULTI_IMSI_SUPPORT**, enable support for SIM-cards with multiple IMSI. Switching the IMSI requires sometimes longer search times. Sets default search timeout to 10 minutes. **Note:** using multiple IMSI cards in combination with LTE-M/NB-IoT mixed mode may cause trouble. The modem start to search in one mode (e.g. NB-IoT), if the timeout of the multi IMSI is short, then the IMSI changes and the modem restarts the search. That may cause the modem to never switch to the second mode. 

- **MODEM_SEARCH_TIMEOUT**, modem network search timeout.

### Power Saving

- **SUSPEND_3V3**, suspend the 3.3V supply, when sleeping.

- **SUSPEND_UART**, suspend the UART, when sleeping.

### GPS/GNSS (experimental)

- **LOCATION_ENABLE**, enable the GPS/GNSS support. Still experimental. Requires a lot of work. Default disabled.

- **LOCATION_ENABLE_CONTINUES_MODE**, enable to continously receive GPS/GNSS signals. With that the Thingy:91 receives the best positions but also requires the most energy (50mA). Default enabled.

- **LOCATION_ENABLE_TRIGGER_MESSAGE**, send a message, when a position is reported by GPS/GNSS. The not continues mode is still experimental. It the most cases, it stops working after something as 30 minutes and requires also 30 minutes to wokr again. So it still requires a lot of work. Default disabled.

**NOTE:** Using GPS/GNSS without assistance data requires to place the `Thingy:91` with free sight to the sky to start. Once the first statelites are detected and the UTC time is available, the `Thingy:91` is able to optimize the GNSS receiving and continues to work also with less signal strength. In my experience, the very best results are achieved, when the GPS/GNSS is mostly on. That requires unfortunately 50mA and so works only for about 20h without charging. 

### Sensors

- **ADP536X_POWER_MANAGEMENT**, enable Thingy's battery power management. Reports battery status (e.g. charging) and level (e.g. 70%). Default enabled.

- **ADP536X_POWER_MANAGEMENT_LOW_POWER**, enable sleeping mode for the Thingy's battery power management. Default enabled.

- **BATTERY_VOLTAGE_SOURCE**, select battery voltage source.

-    **BATTERY_VOLTAGE_SOURCE_ADP536X**, use Thingy's battery power management as voltage source.

-    **BATTERY_VOLTAGE_SOURCE_ADC**, use ADC as voltage source.

-    **BATTERY_VOLTAGE_SOURCE_MODEM**, use modem as voltage source.

- **MOTION_DETECTION**, use ADXL362 (Thingy:91) or LIS2DH (feather nRF9160) to detect, if the device is moved. Only rudimentary function. Default disabled.

- **MOTION_DETECTION_LED**, use green LED to signal detected move. Default disabled.

### Environment Sensors

- **TEMPERATURE_OFFSET**, self-heating temperature offset. Default depends on selected sensor. Supported for `BME680`, `SHT3xD`, and `DS18B20`.  

#### BME680

The `Thingy:91` is equiped with an environment sensor [BME680 (Bosch)](https://www.bosch-sensortec.com/products/environmental-sensors/gas-sensors/bme680/). This sensor is mounted on the backside of the board towards the battery.

**NOTE:** During charging, the battery reaches easily 30° and the reported temperature reflects more this internal temperature than the environment. The enclosure of the `Thingy:91` seems also to decouple the sensor inside from the environment. Therefore either removing the board from the enclosure or using an external sensor may provide better values.

For the [BME680 (Bosch)](https://www.bosch-sensortec.com/products/environmental-sensors/gas-sensors/bme680/) two drivers are available:

- **zephyr bme680 driver**, access to temperature, humidity, pressure, and the "gas sensor resistance". No additional source are required. Enabled at `Zephyr Kernel > Device Drivers > Sensor Drivers > BME680 sensor` (since NCS 2.1.0 already enabled by the device tree).

- **Bosch bme680 BSEC library** (Bosch Sensortec Environmental Cluster (BSEC 1.x)), access to temperature, humidity, pressure, and the IAQ (Index Air Quality).
The [BSEC library, v1.4.9.2](https://www.bosch-sensortec.com/software-tools/software/bsec/) must be downloaded from this Bosch Website and comes with its own [license](https://www.bosch-sensortec.com/media/boschsensortec/downloads/bsec/2017-07-17_clickthrough_license_terms_environmentalib_sw_clean.pdf). Unzip the downloaded archive into the `zephyr-coaps-client/nrf/ext/` (or `<ncs>/nrf/ext/`). The resulting path must be `nrf/ext/bsec_1-4-9-2_generic_release`. The [BME68x-Sensor-API](https://github.com/BoschSensortec/BME68x-Sensor-API) is also required and could be [downloaded from github](https://github.com/BoschSensortec/BME68x-Sensor-API/archive/refs/heads/master.zip). Unzip this as well and copy the files `bme68x.c`, `bme68x.h`, and `bme68x_defs.h` additionally into the folder `nrf/ext/bsec_1-4-9-2_generic_release/examples`. If the preparation is done, disable `Zephyr Kernel > Device Drivers > Sensor Drivers > BME680 sensor` (if enabled), and then enable `Extra Functions > BME680 BSEC` instead.

**Note:** the usage of the IAQ (Index Air Quality) requires a calibration. See "BST-BME680-Integration-Guide-AN008-49.pdf":

"The IAQ accuracy indicator will notify the user when she/he should initiate a calibration process. Calibration is performed in the background if the sensor is exposed to clean or polluted air for approximately 30 minutes each."

(The calibration configuration is not stored for now.)

#### SHT3xD

Zephyr comes also with a driver for the [SHT3xD](https://sensirion.com/de/produkte/katalog/SHT31-DIS-B/) as external sensor.

- **zephyr SHT3XD driver**, access to temperature, humidity. No additional source are required. Requires a device-tree overlay, see [sht3x.overlay](../sht3x.overlay). Enabled at `Zephyr Kernel > Device Drivers > Sensor Drivers > SHT3xD`. (since NCS 2.1.0 already enabled by the overlay).

#### DS180B20

Zephyr comes also with a driver for the [DS180B20](https://www.analog.com/en/products/ds18b20.html) as external sensor.

- **zephyr DS180B20 driver**, access to temperature. No additional source are required. Requires a device-tree overlay, see [one-wire-uart1.overlay](../one-wire-uart1.overlay) or [one-wire-uart2.overlay](../one-wire-uart2.overlay).

## Overlays

As mentioned above, overlays are files, which are used to configure a set of features. 

## List of overlays

[bsec-prj.conf](../bsec-prj.conf) prepares to use **Bosch bme680 BSEC library**, see above. 

[bme-prj.conf](../bme-prj.conf) prepares to use **Bosch bme680 Zephyr Sensor library**, see above. 

[sht3x-prj.conf](../sht3x-prj.conf) prepares to use the **SHT3X Zephyr Sensor library**. Requires device tree overlay [sht3x.overlay](../sht3x.overlay) additionally.

[one-wire-prj.conf](../one-wire-prj.conf) prepares to use the **DS18B20 Zephyr Sensor library**. Requires device tree overlay [one-wire-uart1.overlay](../one-wire-uart1.overlay) or [one-wire-uart2.overlay](../one-wire-uart2.overlay)additionally.

[5min-prj.conf](../5min-prj.conf) prepares to send a message every 5 minutes. Enables to use a environment history to store sensor data.

[10min-prj.conf](../10min-prj.conf) prepares to send a message every 10 minutes. Enables to use a environment history to store sensor data.

[60min-prj.conf](../60min-prj.conf) prepares to send a message every 60 minutes. Enables to use a environment history to store sensor data.

## Apply overlays

Apply configuration overlays:

```
west build -b thingy91_nrf9160_ns --pristine -- -DOVERLAY_CONFIG="bme-prj.conf;60min-prj.conf"
```

With device tree overlay:

```
west build -b thingy91_nrf9160_ns --pristine -- -DOVERLAY_CONFIG="sht3x-prj.conf;60min-prj.conf" -DDTC_OVERLAY_FILE="boards/nrf9160dk_nrf9160_ns.overlay;sht3x.overlay"
```

**Note:** in difference to the "*.conf" files, using a "*.overlay" prevents the board specific project overlay "board/*.overlay" from being used. Therefore add the project board overlay also when using DTC_OVERLAY_FILE.
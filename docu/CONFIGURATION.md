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

The modem firmware versions 1.3.2 to 1.3.4 are also applying HPPLMN searchs for global IMSIs (starting with 9), which consumes a lot of energy. This behavior is disabled again with version 1.3.5. Please update to 1.3.5 or newer when using such global SIM cards. 

The usage of the sensors also reduces that time. Consider to measure less frequent, e.g. every 10 minutes instead of every couple of seconds.

## Basic

- **APPL_MODEL**, application model, used to select the firmware variant for the hardware when downloading via CoAP. The resulting CoAP resource path will be `fw`/`<model>`/`<version>`, e.g. "fw/dk/0.10.0+0".

- **APPL_MODEL_DESCRIPTION**, application description to include in reported version.

- **INIT_SETTINGS**, include configuration values for initial settings. Only applied on the first start after flashing the app with `--chiperase` (see nrfjprog).

- **COAP_SERVER_HOSTNAME**, hostname of the coap/dtls 1.2 cid server. Default is the Californium's sandbox. Only provided, if **INIT_SETTINGS** is enabled. 

- **COAP_SERVER_HOSTNAME**, hostname of the coap/dtls 1.2 cid server. Default is the Californium's sandbox at `californium.eclipseprojects.io`. Only provided, if **INIT_SETTINGS** is enabled.

- **COAP_SERVER_ADDRESS_STATIC**, static ip-address of the coap/dtls 1.2 cid server. Fallback, if DNS isn't setup. Only provided, if **INIT_SETTINGS** is enabled.

- **COAP_SERVER_PORT**, service port for none secure communication. Default `5683`. Only provided, if **INIT_SETTINGS** is enabled.

- **COAP_SERVER_SECURE_PORT**, service port for secure communication. Default `5684`. Only provided, if **INIT_SETTINGS** is enabled.

- **DEVICE_IDENTITY**, the device identiy. `${imei}` will be replaced by the IMEI of the device. Default `cali.${imei}`, e.g. "cali.352656100985434". Only provided, if **INIT_SETTINGS** is enabled.

- **PROVISIONING_GROUP**, device group for provisioning. Default "Auto". Only provided, if **INIT_SETTINGS** is enabled.

- **DTLS_PSK_IDENTITY**, the PSK identiy. `${imei}` will be replaced by the IMEI of the device. May differ from the `DEVICE_IDENTITY`. Default `cali.${imei}`, e.g. "cali.352656100985434". Only provided, if **INIT_SETTINGS** is enabled.

- **DTLS_PSK_SECRET_GENERATE**, initially generate the PSK secret. Only provided, if **INIT_SETTINGS** is enabled.

- **DTLS_PSK_SECRET**, the PSK secret. In base 64 as text `"base64"`, ASCII as text with additional single quotes `"'ascii'"`, or hexadecimal as text with `":0x"` prefix, e.g. `":0x1234abcd"`. Default `"'.fornium'"`, the sandbox uses this secret for the a wildcard identity "cali.*". This is only intended to easy access the sandbox, don't use it on your own test setup. Only provided, if **INIT_SETTINGS** is enabled.

- **DTLS_ECDSA_PRIVATE_KEY_GENERATE**, initially generate the ECDSA key-pair. Only provided, if **INIT_SETTINGS** is enabled.

- **DTLS_ECDSA_PRIVATE_KEY**, ECDSA device private key. Only SECP256R1 is supported. The private key is provided plain (32 bytes) without the algorithm ids of in ASN.1 encoding with algorithm header. The public key is not provided, it will be derived from the private key. Hexadecimal encoded with prefix ":0x", or base64 as "". Only provided, if **INIT_SETTINGS** is enabled.

- **DTLS_ECDSA_TRUSTED_PUBLIC_KEY**, ECDSA trusted server public key. Only SECP256R1 is supported. Either encoded as ASN.1 (including algorith header) or plain concated public_x and public_y key (64 bytes). Hexadecimal encoded with prefix ":0x", or base64 as "". Only provided, if **INIT_SETTINGS** is enabled.

- **DTLS_ECDSA_AUTO_PROVISIONING** enable auto provisioning using ECDSA provisioning key-pair. Only provided, if **COAP_SERVICE** is enabled.

- **DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY**, ECDSA provisioning device private key. Only SECP256R1 is supported. The private key is provided plain (32 bytes) without the algorithm ids of in ASN.1 encoding with algorithm header. The public key is not provided, it will be derived from the private key. Hexadecimal encoded with prefix ":0x", or base64 as "". Only provided, if **INIT_SETTINGS** is enabled.

- **DTLS_ALWAYS_HANDSHAKE**, enables to use a DTLS handshake for each request. Using DTLS 1.2 Connection ID obsoletes such frequent handshakes.

- **COAP_WAIT_ON_POWERMANAGER**, coap waits for the power-manager to start exchanging application data. Takes up to 30 s and delays the first exchange. Default disabled.

- **COAP_SEND_INTERVAL**, coap send interval in seconds. Used, if messages are send frequently. Default 0s, disabled.

- **COAP_FAILURE_SEND_INTERVAL**, coap send interval after failures in seconds. 0 to use send interval.


- **COAP_SEND_MODEM_INFO**, include information from the modem (`send_flag` 0x10).

```
108 [s], Thingy:91 v0.7.108+1 (2.4.2), 0*1, 1*0, 2*0, 3*0, failures 0
HW: B1A, MFW: 1.3.5, IMEI: ???????????????
!4985 mV
Last code: 2023-09-20T11:08:36Z reboot cmd
```

- **COAP_SEND_SIM_INFO**, include information from the SIM-card (`send_flag` 0x20).

```
ICCID: ????????????????????, eDRX cycle: off, HPPLMN interval: 10 [h]
IMSI: ???????????????
```

- **COAP_SEND_NETWORK_INFO**, include network information (`send_flag` 0x40).

```
Network: CAT-M1,roaming,Band 20,#PLMN 26202,TAC 47490,Cell ????????,EARFCN 6300
PDN: ???.?????.??,???.???.???.???,rate-limit 256,86400 s
PSM: TAU 90000 [s], Act 0 [s], AS-RAI, Released: 2032 ms
```

- **COAP_SEND_STATISTIC_INFO**, include statistic information. e.g. transmissions and network searchs (`send_flag` 0x80).

```
!CE: down: 8, up: 1, RSRP: -114 dBm, CINR: -1 dB, SNR: 0 dB
Stat: tx 1 kB, rx 0 kB, max 748 B, avg 146 B
Cell updates 1, Network searchs 1 (3 s), PSM delays 0 (0 s), Restarts 0
Wakeups 1, 1 s, connected 7 s, asleep 0 s
```

- **COAP_SEND_MINIMAL**, dynamically reduce information in all topics above.

- **COAP_NO_RESPONSE_ENABLE**, send one-way coap message (request without response).

- **COAP_RESOURCE**, resource name of request. `${imei}` will be replaced by the IMEI of the device.Default "echo". Only provided, if **INIT_SETTINGS** is enabled.

- **COAP_QUERY**, query of request. Must start with `?`. `${imei}` will be replaced by the IMEI of the device. Only provided, if **INIT_SETTINGS** is enabled.

## URI query parameter

### Supported on Californium Sandbox:
- **delay=<n>**, delay response by n sec.
- **rlen=<n>**, response length n bytes.
- **ack**,  use separate response.
- **keep** keep request on server.
- **id=<device-id>** device id, if PSK is not used.

### Supported on Californium CoAP-S3-Proxy:
- **series**,  enable series function of s3-proxy.
- **forward**, enable http-proxy forward.
- **read[=<subpath>]**, read sub-resource. Default subpath is "config".
- **write[=<subpath>]** write sub-resource. Default subpath is "${now}".

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

- **UDP_PSM_CONNECT_RAT**, requested active time during connect. Some extra time to receive maverick messages.

- **UDP_PSM_RETRANS_RAT**, requested active time when retransmission are used. Some extra time to receive maverick messages.

- **UDP_EDRX_ENABLE**, enable eDRX.

- **STATIONARY_MODE_ENABLE**, enable stationary mode. Optimize power consumption, if device is sationary at one place and is not moved.

- **MODEM_SAVE_CONFIG_THRESHOLD**, threshold to save the modem configuration. The modem is switched off and on to save the configuration.

- **MODEM_MULTI_IMSI_SUPPORT**, enable support for SIM-cards with multiple IMSI. Switching the IMSI requires sometimes longer search times. Sets default search timeout to 10 minutes. **Note:** using multiple IMSI cards in combination with LTE-M/NB-IoT mixed mode may cause trouble. The modem start to search in one mode (e.g. NB-IoT), if the timeout of the multi IMSI is short, then the IMSI changes and the modem restarts the search. That may cause the modem to never switch to the second mode. 

- **MODEM_ICCID_LTE_M_PREFERENCE**, list of ICCID prefixes (5 digits) to use LTE-M preference.

- **MODEM_ICCID_NBIOT_PREFERENCE**, list of ICCID prefixes (5 digits) to use NB-IoT preference.

- **MODEM_FAULT_THRESHOLD**, threshold for modem faults per week to trigger a modem reboot.

- **PROTOCOL_CONFIG_SWITCH**, enable config switches to select the protocol. coap (coap over plain UDP) and coaps (coap over DTLS / UDP) are supported. 

- **PROTOCOL_MODE**, select the protocol.

### Extras

- **UART_RECEIVER**, enable UART command recevier.

- **UART_UPDATE**, enable firmware update using UART and XMODEM.

- **COAP_UPDATE**, enable firmware update using CoAP.

- **SH_CMD**, enable sh-cmds.

- **SH_CMD_UNLOCK**, enable protected sh-cmds.

- **SH_CMD_UNLOCK_PASSWORD**, password to unlock protected sh-cmds. Only provided, if **INIT_SETTINGS** is enabled.

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

- **BATTERY_ADC**, enable ADC battery voltage measurement.

- **EXT_BATTERY_ADC**, enable external ADC battery voltage measurement. Monitors a second external battery. Requires [vbatt2.overlay](../vbatt2.overlay).

- **BATTERY_TYPE_LIPO_1350_MAH**, include LiPo 1350mAh profile, Thingy:91 internal.

- **BATTERY_TYPE_LIPO_2000_MAH**, include LiPo 2000mAh profile.

- **BATTERY_TYPE_NIMH_2000_MAH**, include NiMh 2000 mAh (Eneloop) profile.

- **BATTERY_TYPE_DEFAULT**, set default battery type. 0 := no battery, 1 := LiPO 1350 mAh (Thingy:91), 2 := LiPO 2000 mAh (nRF9160 feather), 3 := NiMH 2000 mAh (nRF9160 feather). Only provided, if **INIT_SETTINGS** is enabled.

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

[prov-prj.conf](../prov-prj.conf) prepares to generate an initial image including the setting values. 

[bsec-prj.conf](../bsec-prj.conf) prepares to use **Bosch bme680 BSEC library**, see above. 

[bme-prj.conf](../bme-prj.conf) prepares to use **Bosch bme680 Zephyr Sensor library**, see above. 

[sht3x-prj.conf](../sht3x-prj.conf) prepares to use the **SHT3X Zephyr Sensor library**. Requires device tree overlay [sht3x.overlay](../sht3x.overlay) additionally.

[one-wire-prj.conf](../one-wire-prj.conf) prepares to use the **DS18B20 Zephyr Sensor library**. Requires device tree overlay [one-wire-uart1.overlay](../one-wire-uart1.overlay) or [one-wire-uart2.overlay](../one-wire-uart2.overlay) additionally.

[5min-prj.conf](../5min-prj.conf) prepares to send a message every 5 minutes. Enables to use a environment history to store sensor data.

[10min-prj.conf](../10min-prj.conf) prepares to send a message every 10 minutes. Enables to use a environment history to store sensor data.

[60min-prj.conf](../60min-prj.conf) prepares to send a message every 60 minutes. Enables to use a environment history to store sensor data.

[uart-prj.conf](../uart-prj.conf) prepares UART for logging and firmware update via XMODEM.

[nouart-prj.conf](../uart-prj.conf) disables loggnin and firmware update via XMODEM.

[hivescale-prj.conf](../hivescale-prj.conf) prepares to use the **NAU7802 I2C ADC**. Requires device tree overlay [hivescale-feather.overlay](../hivescale-feather.overlay) or [hivescale-dk.overlay](../hivescale-dk.overlay) additionally.

[vbatt2.overlay](../vbatt2.overlay) prepares to monitor a second, external batters.

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
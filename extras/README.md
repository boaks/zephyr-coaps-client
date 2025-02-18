![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with Eclipse/TinyDtls

# New/Upcoming Board Support
 
## Upcoming Board Support - nRF9161-feather

| [Circuit Dojo, nRF9161 feather - Suggestions for next version](https://community.circuitdojo.com/d/480-nrf9160-feather-suggestions-for-next-version/64) | [<img src="../docu/nRF9161-feather.png" width="450"/>](../docu/nRF9161-feather.png) |
| :- | - |

**Note:** The board files are available from `Circuit Dojo`, only the `v2.7.x` files will be maintained by `Circuit Dojo`.

### Upcoming Board Support - nRF9161-feather - NCS 2.9.0

Select version `v2.7.x` for the [circuitdojo/nrf9160-feather-examples-and-drivers](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers) repository and download the [ZIP v2.7.x](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers/archive/refs/heads/v2.7.x.zip). Extract the files and copy the content of `boards/circuitdojo/` into `zephyr/boards/circuitdojo`.

```
cp -r v2.9.0/circuitdojo ../../zephyr/boards/
```

**Note:** If the NCS 2.6.2 board files have been installed before, that directory `zephyr/boards/arm/circuitdojo_feather_nrf9161` must be removed.

The mcuboot configuration is already added in the project's `sysbuild/mcuboot/boards` folder.

### Upcoming Board Support - nRF9161-feather - NCS 2.6.2

Select version `v2.6.x` for the [circuitdojo/nrf9160-feather-examples-and-drivers](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers) repository and download the [ZIP v2.6.x](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers/archive/refs/heads/v2.6.x.zip). Extract the files and copy the content of `boards/arm/circuitdojo_feather_nrf9161/` into `zephyr/boards/arm/circuitdojo_feather_nrf9161`.

```
cp -r v2.6.2/circuitdojo_feather_nrf9161 ../../zephyr/boards/arm/
```

and the mcuboot configuration `mcuboot/circuitdojo_feather_nrf9161.conf` into the bootloader zephyr's boards folder `bootloader/mcuboot/boot/zephyr/boards`

```
cp v2.6.2/mcuboot/circuitdojo_feather_nrf9161.conf ../../bootloader/mcuboot/boot/zephyr/boards
```

To flash the nRF9161-feather use 

```
west flash --runner pyocd
```

or

```
# v2.6.2
pyocd load --target nRF9160_xxAA --format hex build/zephyr/merged.hex

# v2.9.0
pyocd load --target nRF9160_xxAA --format hex build/merged.hex
```

Unfortunately, this reports randomly, from time to time

```
W nRF9160_xxAA APPROTECT enabled: will try to unlock via mass erase [target_nRF91]
```

which will then rest all "settings". 

### Power rails

The nRF9161-feather has an RP20240 for USB and flashing. The BUCK2 provides power to that. In order to reduce the quiescent current, it's required to disable the RP2040, but then the board has no USB serial interfaces and it's not possible to flash it. Therefore the application detects on startup, if USB is connected. If so, the RP20240 is kept powered. If no USB is detected, the RP20240 is switched off. Therefore

- If you want to check the power consumption over time, please reset the device with unpluged USB.
- If you want to flash the device, connect USB and reset it.

## New Board Support - Conexio Stratus Pro

| [Conexio Stratus Pro nRF9161](https://conexiotech.com/conexio-stratus-pro-nrf9161/) | [<img src="https://conexiotech.com/wp-content/uploads/2024/02/D-copy.png" width="450"/>](https://conexiotech.com/wp-content/uploads/2024/02/D-copy.png) |
| :- | - |
| [Conexio Stratus Pro nRF9151](https://conexiotech.com/conexio-stratus-pro-nrf9151/) | [<img src="https://conexiotech.com/wp-content/uploads/2024/10/stratus-pro-transparency-1.png" width="450"/>](https://conexiotech.com/wp-content/uploads/2024/10/stratus-pro-transparency-1.png) |

**Note:** The board files are available from [Conexio Technologies](https://docs.conexiotech.com/master/building-and-programming-an-application/conexio-stratus-board-definition-files#step-2-patch-mcuboot-file-for-stratus-pro-board).  Only the `v2.7.x` files are maintained by `Conexio Technologies`.

### New Board Support - Conexio Stratus Pro - NCS 2.9.0

Download the [Conexio Firmware SDK](https://github.com/Conexiotechnologies/conexio-firmware-sdk/archive/refs/heads/main.zip), unzip it and copy the folder `boards/conexio` into the zephyr's boards folder `boards/conexio`

```
cp -r v2.9.0/conexio ../../zephyr/boards/
```

The mcuboot configuration is already added in the project's `sysbuild/mcuboot/boards` folder.

**Note:** If the NCS 2.6.2 board files have been installed before, that directory `zephyr/boards/arm/conexio_stratus_pro` must be removed.

### New Board Support - Conexio Stratus Pro - NCS 2.6.2

**Note:** the NCS 2.6.2 support offered in folder `conexio_stratus_pro` is derived from the `Conexio Technologies board files`.

To build the coaps-client with Conexio Stratus Pro support, first cp the folder `conexio_stratus_pro` into the zephyr's boards folder `boards/arm/conexio_stratus_pro`

```
cp -r v2.6.2/conexio_stratus_pro ../../zephyr/boards/arm/
```

and the mcuboot configuration `mcuboot/conexio_stratus_pro.conf` into the bootloader zephyr's boards folder `bootloader/mcuboot/boot/zephyr/boards`

```
cp v2.6.2/mcuboot/conexio_stratus_pro.conf ../../bootloader/mcuboot/boot/zephyr/boards
```



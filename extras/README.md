![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with Eclipse/TinyDtls

# New/Upcoming Board Support
 
## Upcoming Board Support - nRF9161-feather

| [Circuit Dojo, nRF9161 feather - Suggestions for next version](https://community.circuitdojo.com/d/480-nrf9160-feather-suggestions-for-next-version/64) | [<img src="../docu/nRF9161-feather.png" width="450"/>](../docu/nRF9161-feather.png) |
| :- | - |

**Note:** The maintained board files will be available from `Circuit Dojo`.

To build the coaps-client with nRF9161-feather support, first cp the folder `circuitdojo_feather_nrf9161` into the zephyr's boards folder `boards/arm`

```
cp -r circuitdojo_feather_nrf9161 ../../zephyr/boards/arm/
```

and the mcuboot configuration `mcuboot/circuitdojo_feather_nrf9161.conf` into the bootloader zephyr's boards folder `bootloader/mcuboot/boot/zephyr/boards`

```
cp mcuboot/circuitdojo_feather_nrf9161.conf ../../bootloader/mcuboot/boot/zephyr/boards
```

To flash the nRF9161-feather use 

```
west flash --runner pyocd
```

or

```
pyocd load --target nRF9160_xxAA --format hex build/zephyr/merged.hex
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

**Note:** The maintained board files are available from [Conexio Technologies](https://docs.conexiotech.com/master/building-and-programming-an-application/conexio-stratus-board-definition-files#step-2-patch-mcuboot-file-for-stratus-pro-board).

To build the coaps-client with Conexio Stratus Pro support, first cp the folder `conexio_stratus_pro` into the zephyr's boards folder `boards/arm`

```
cp -r conexio_stratus_pro ../../zephyr/boards/arm/
```

and the mcuboot configuration `mcuboot/conexio_stratus_pro.conf` into the bootloader zephyr's boards folder `bootloader/mcuboot/boot/zephyr/boards`

```
cp mcuboot/conexio_stratus_pro.conf ../../bootloader/mcuboot/boot/zephyr/boards
```


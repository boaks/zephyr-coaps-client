![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with Eclipse/TinyDtls

# New/Upcoming Board Support
 
## New Board Support - nRF9151 feather

| [Circuit Dojo, nRF9151 feather](https://www.circuitdojo.com/products/nrf9151-feather) | [<img src="https://www.circuitdojo.com/base/api/files/products/515ssdvlohrn7es/n_rf9151_feather_front_centered_zVodQCvzhu.png" width="450"/>](https://www.circuitdojo.com/base/api/files/products/515ssdvlohrn7es/n_rf9151_feather_front_centered_zVodQCvzhu.png) |
| :- | - |

**Note:** The board files are available from `Circuit Dojo`, only the `v2.7.x` files are maintained by `Circuit Dojo`.

### New Board Support - nRF9151 feather - NCS 2.9.x

Select version `v2.7.x` for the [circuitdojo/nrf9160-feather-examples-and-drivers](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers) repository and download the [ZIP v2.7.x](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers/archive/refs/heads/v2.7.x.zip). Extract the files and copy the content of `boards/circuitdojo/` into `zephyr/boards/circuitdojo`.

```
cp -r v2.9.x/circuitdojo ../../zephyr/boards/
```

The mcuboot configuration is already added in the project's `sysbuild/mcuboot/boards` folder.

For flashing a nRF9151 feather see [Flashing New Boards With OCD Support ](#flashing_new_boards_with_ocd_support).

## New Board Support - Conexio Stratus Pro

| [Conexio Stratus Pro nRF9161](https://conexiotech.com/conexio-stratus-pro-nrf9161/) | [<img src="https://conexiotech.com/wp-content/uploads/2024/02/D-copy.png" width="450"/>](https://conexiotech.com/wp-content/uploads/2024/02/D-copy.png) |
| :- | - |
| [Conexio Stratus Pro nRF9151](https://conexiotech.com/conexio-stratus-pro-nrf9151/) | [<img src="https://conexiotech.com/wp-content/uploads/2024/10/stratus-pro-transparency-1.png" width="450"/>](https://conexiotech.com/wp-content/uploads/2024/10/stratus-pro-transparency-1.png) |

**Note:** The board files are available from [Conexio Technologies](https://docs.conexiotech.com/master/building-and-programming-an-application/fetch-conexio-firmware-sdk-and-board-definition-files).  Only the `v2.7.x` files are maintained by `Conexio Technologies`. 


https://docs.conexiotech.com/master/building-and-programming-an-application/fetch-conexio-firmware-sdk-and-board-definition-files
### New Board Support - Conexio Stratus Pro - NCS 2.9.x

Download the [Conexio Firmware SDK](https://github.com/Conexiotechnologies/conexio-firmware-sdk/archive/refs/heads/main.zip), unzip it and copy the folder `boards/conexio` into the zephyr's boards folder `boards/conexio`

```
cp -r v2.9.x/conexio ../../zephyr/boards/
```

The mcuboot configuration is already added in the project's `sysbuild/mcuboot/boards` folder.

**Note:** If the NCS 2.6.x board files have been installed before, that directory `zephyr/boards/arm/conexio_stratus_pro` must be removed.

### New Board Support - Conexio Stratus Pro - NCS 2.6.x

**Note:** the NCS 2.6.x board files in folder `conexio_stratus_pro` are derived from the `Conexio Technologies board files` and not intended to be supported in the future. See [Conexio Technologies](https://docs.conexiotech.com/master/building-and-programming-an-application/conexio-stratus-board-definition-files) for NCS 2.6.x.

To build the coaps-client with Conexio Stratus Pro support, first cp the folder `conexio_stratus_pro` into the zephyr's boards folder `boards/arm/conexio_stratus_pro`

```
cp -r v2.6.x/conexio_stratus_pro ../../zephyr/boards/arm/
```

and the mcuboot configuration `mcuboot/conexio_stratus_pro.conf` into the bootloader zephyr's boards folder `bootloader/mcuboot/boot/zephyr/boards`

```
cp v2.6.x/mcuboot/conexio_stratus_pro.conf ../../bootloader/mcuboot/boot/zephyr/boards
```

## New Board Support - nRF9151 Connect Kit - makerdiary

| [Makerdiary nRF9151 Connect Kit](https://makerdiary.com/products/nrf9151-connectkit/) | [<img src="https://github.com/makerdiary/nrf9151-connectkit/blob/main/docs/assets/images/attaching_lte_antenna.png" width="450"/>](https://github.com/makerdiary/nrf9151-connectkit/blob/main/docs/assets/images/attaching_lte_antenna.png) |
| :- | - |

Download the [Makerdiary nRF9151 Connect Kit SDK](https://github.com/makerdiary/nrf9151-connectkit/archive/refs/heads/main.zip), unzip it and copy the folder `boards/makerdiary/nrf9151_connectkit` into the zephyr's boards folder `boards/makerdiary`

The mcuboot configuration is already added in the project's `sysbuild/mcuboot/boards` folder.

For flashing a nRF9151 Connect Kit see [Flashing New Boards With OCD Support ](#flashing_new_boards_with_ocd_support).

## Flashing New Boards With OCD Support 

The `nRF9151 feather`, `nRF9161 feather` and `nRF9151 Connect Kit` comes with OCD based on CMSIS-DAP.

To flash such a device use either `pyocd` or `probe-rs`. It may be required to adjust the permissions of those USB device by copying the `udev-rules` into `/etc/udev/rules.d`.

Using `west flash` and `pyocd`: 

```
west flash --runner pyocd
```

or `pyocd` directly:

```
# v2.6.x
pyocd load --target nRF9160_xxAA --format hex build/zephyr/merged.hex

# v2.9.x
pyocd load --target nRF9160_xxAA --format hex build/merged.hex
```

Unfortunately, this reports randomly, from time to time

```
W nRF9160_xxAA APPROTECT enabled: will try to unlock via mass erase [target_nRF91]
```

which will then rest all "settings". 

In the case, it is intended to remove the current settings and initialize them with newly provided data, use

```
# v2.6.x
pyocd load --target nRF9160_xxAA --format hex build/zephyr/merged.hex -e chip

# v2.9.x
pyocd load --target nRF9160_xxAA --format hex build/merged.hex -e chip
```


It is also possible to update the modem firmware.

```
pyocd cmd --target nRF9160_xxAA -c 'nrf91-update-modem-fw -f mfw_nrf91x1_2.0.2.zip'
```


Alternatively you may use `probe-rs `:

```
# v2.6.x
probe-rs download --chip nRF9160_xxAA --binary-format hex build/zephyr/merged.hex

# v2.9.x
probe-rs download --chip nRF9160_xxAA --binary-format hex build/merged.hex

probe-rs reset --chip nRF9160_xxAA
```

In some case, `probe-rs` reports

```
 WARN probe_rs::vendor::nordicsemi::sequences::nrf: Core 0 is locked. Erase procedure will be started to unlock it.
Error: Connecting to the chip was unsuccessful.

Caused by:
    0: An ARM specific error occurred.
    1: An operation could not be performed because it lacked the permission to do so: erase_all
```

then it's required to add `--allow-erase-all`, but that remove then the settings either.

```
# v2.6.x
probe-rs download --chip nRF9160_xxAA --binary-format hex build/zephyr/merged.hex --allow-erase-all

# v2.9.x
probe-rs download --chip nRF9160_xxAA --binary-format hex build/merged.hex --allow-erase-all

probe-rs reset --chip nRF9160_xxAA
```

Sometimes it also helps to reset the board and retry without `--allow-erase-all`. 

In the case, it is intended to remove the current settings and initialize them with newly provided data, use

```
# v2.6.x
probe-rs download --chip nRF9160_xxAA --chip-erase --binary-format hex build/zephyr/merged.hex --allow-erase-all

# v2.9.x
probe-rs download --chip nRF9160_xxAA --chip-erase --binary-format hex build/merged.hex --allow-erase-all

probe-rs reset --chip nRF9160_xxAA
```

## Beta Board Support - nRF9161 feather (discontinued, replaced by nRF9151 feather) 

| [Circuit Dojo, nRF9161 feather - Suggestions for next version](https://community.circuitdojo.com/d/480-nrf9160-feather-suggestions-for-next-version/64) | [<img src="../docu/nRF9161-feather.png" width="450"/>](../docu/nRF9161-feather.png) |
| :- | - |

**Note:** The board files are available from `Circuit Dojo`.

### Beta Board Support - nRF9161 feather - NCS 2.9.x

Select version `v2.7.x` for the [circuitdojo/nrf9160-feather-examples-and-drivers](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers) repository and download the [ZIP v2.7.x](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers/archive/refs/heads/v2.7.x.zip). Extract the files and copy the content of `boards/circuitdojo/` into `zephyr/boards/circuitdojo`.

```
cp -r v2.9.x/circuitdojo ../../zephyr/boards/
```

**Note:** If the NCS 2.6.x board files have been installed before, that directory `zephyr/boards/arm/circuitdojo_feather_nrf9161` must be removed.

The mcuboot configuration is already added in the project's `sysbuild/mcuboot/boards` folder.

### Beta Board Support - nRF9161-feather - NCS 2.6.x

Select version `v2.6.x` for the [circuitdojo/nrf9160-feather-examples-and-drivers](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers) repository and download the [ZIP v2.6.x](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers/archive/refs/heads/v2.6.x.zip). Extract the files and copy the content of `boards/arm/circuitdojo_feather_nrf9161/` into `zephyr/boards/arm/circuitdojo_feather_nrf9161`.

```
cp -r v2.6.x/circuitdojo_feather_nrf9161 ../../zephyr/boards/arm/
```

and the mcuboot configuration `mcuboot/circuitdojo_feather_nrf9161.conf` into the bootloader zephyr's boards folder `bootloader/mcuboot/boot/zephyr/boards`

```
cp v2.6.x/mcuboot/circuitdojo_feather_nrf9161.conf ../../bootloader/mcuboot/boot/zephyr/boards
```

For flashing a nRF9161 feather see [Flashing New Boards With OCD Support ](#flashing_new_boards_with_ocd_support).

### Power rails

The nRF9161-feather has an RP20240 for USB and flashing. The BUCK2 provides power to that. In order to reduce the quiescent current, it's required to disable the RP2040, but then the board has no USB serial interfaces and it's not possible to flash it. Therefore the application detects on startup, if USB is connected. If so, the RP20240 is kept powered. If no USB is detected, the RP20240 is switched off. Therefore

- If you want to check the power consumption over time, please reset the device with unpluged USB.
- If you want to flash the device, connect USB and reset it.


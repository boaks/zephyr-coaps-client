![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

** !!! Under Construction !!! **

# Cellular Explorer

The 
| [Nordic Semiconductor, Thingy:91](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91) | ![Thingy:91](./thingy91.jpg) |
| :- | - |

comes with an Bluetooth Low Energy interface, that allows to connect the `Thingy:91` to a smartphone and send locally commands to the `Thingy:91`. That helps to analyze network situations even without valid cellular connectivity of your SIM card.

In order to enable this function, the [at-cmd-prj.conf](../at-cmd-prj.conf) must be used to build the app.

Other devices without Bluetooth Low Energy interface may also be used via a USB serial. The

| [Circuit Dojo, nRF9160 feather v5](https://www.jaredwolff.com/store/nrf9160-feather/) | ![nRF9160-feather-v5](https://docs.jaredwolff.com/img/nrf9160-feather-v4-nobg.jpg) |
| :- | - |

comes with a USB-C plug and if the smartphone is also equipped with USB-C, a simple USB-C to USB-C wire will do it. Not that comfortable as  Bluetooth Low Energy, but it works.

For some more sophisticated tests, e.g. adapting some lists on the SIM card, it may be easier to use a PC via a USB serial and open a terminal on that PC.

## Enable Bluetooth Low Energy

If you connect the `Thingy:91` via USB to your PC, a "local USB drive" pops up.

![local USB drive](./local_usb_drive.png).

Edit the "Config.txt" file.

```
==========================================
  Nordic Thingy:91 Configuration options
==========================================
The parameters below can be changed at runtime.

NOTE: For changes to take effect,
safely disconnect (unmount) the drive and disconnect the USB cable.
==========================================
BLE_ENABLED=1
BLE_NAME=Thingy:91-2 UART
```

Enable BLE by changing the `BLE_ENABLED=0` to `BLE_ENABLED=1`. Also consider to change the `BLE_NAME=Thingy:91 UART` to a rather unique name. Save your changes.

## Smartphone App

In order to communication with the `Thingy:91` via Bluetooth Low Energy you need a App which supports the `Nordic BLE UART` service.

For Android the "Serial Bluetooth Terminal 1.43" from [Kai Morich](http://www.kai-morich.de/android) works very well.

For iOS the "Bluefruit Connect" works.

If you want to use the USB serial, the "Serial USB Terminal 1.43" from [Kai Morich](http://www.kai-morich.de/android) works also very well.

Install the App and connect the `Thingy:91`.

## Usage

The most [nRF9160 AT-commands](https://infocenter.nordicsemi.com/pdf/nrf9160_at_commands_v2.3.pdf) are supported. Additionally some shortcuts and extra functions are available as custom commands. For simple usage, these custom commands uses simple characters and digits, instead of control characters as '+', '%' or '='. Type `help` and press the "send button" ![arrow](./serial_bluetooth_terminal_send_small.jpg):

{#help}
![help](./serial_bluetooth_terminal_help.jpg)

(The screenshot shows only the list of commands in August 2023, the current list may contain more commands.)

If the smart phone App offers macros, add you favorite AT-commands or commands as macro.

## Explore the Cellular Network

Hopefully in the most areas the device should be able to connect to the network and to exchange data with the cloud. To test that, type `send` and press the "send button" ![arrow](./serial_bluetooth_terminal_send_small.jpg):

![send](./serial_bluetooth_terminal_send_scan.jpg)

If that doesn't work or if you want to see, which cellular networks are available at your location, then type `scan` and press the "send button" ![arrow](./serial_bluetooth_terminal_send_small.jpg)

The displayed list of networks will vary. As default, `scan` measure the currently already known channels. If you want to search for new channels, provide additional parameters. For such cases, a command comes also with a specific help. Type `help scan` and press the "send button" ![arrow](./serial_bluetooth_terminal_send_small.jpg) (see [screenshot above](#help)). 

To perform a scan with searching for new channels, use `scan 5 <n>` where `n` is the maximum expected number of networks. It will take a while. The provided parameters will become the new default. if you decide to use `scan 5 6`, the next execution of `scan` will also use these parameters.

![search](./serial_bluetooth_terminal_search.jpg)

In some case you may get aware, that your modem connects to a network with less signal strenght than other available networks. With the `net` command you see, to which provider (the [PLMN](https://mcc-mnc.com/)) the device is connected and which technology is used, `LTE-M` or `NB-IoT`. If you want to test the communication with an other provider and/or switching the technology, use to `con` command. Also this command has a specific help.

![help cfg](./serial_bluetooth_terminal_help_con.jpg)

To switch the network operator (e.g. `26202`) and technology (`m1` or `nb`), use `con 26202 m1`.

![cfg](./serial_bluetooth_terminal_con.jpg)

that requires some time. If the network operator rejects your SIM, you need to `con` to an other network operator again.

## Locating a Device

Sometimes it may be interesting to know the current location of the device. If the App doesn't use GPS, you still may be able to get a rough location using the base station as reference. There are several web-sites which offers a service for that. One service fro non-commercial usage is [cellmapper](https://www.cellmapper.net).

You will need the PLMN and the tower code (Cell devided by 256), e.g. PLMN `24008` and Cell `28609803` resulting in `111757`. Open the [cellmapper](https://www.cellmapper.net) and first set the PLMN using `Menu` -> `Provider`. 

![CellMapper - Provider](./cellmapper_provider.png)

Then close the `Provider Menu` and open the `Search Menu`. 

![CellMapper - Search](./cellmapper_search.png) 

enter the calculated tower number and press `<return>`. I dialog will show up and, if the position of the tower is available, the entry of that tower will be shown.

![CellMapper - Search](./cellmapper_result.png) 

Select the entry (`eNB <tower number>`) and a map will be opened showing the position of the base station.

## Test Functions and Parameter of a Cellular Network

If you have connected the device with the provider and technology you want to investigate, you may check with `net`, which functions are supported.

```
> net

I 60.855: Network: CAT-M1,roaming,Band 20,PLMN 26202,TAC 47490,Cell 52262913,EARFCN 6300
I 60.855: PDN: flolive.net,100.64.55.203
I 60.856: PSM: TAU 90000 [s], Act 0 [s], AS-RAI, Released: 2015 ms
I 60.856: !CE: down: 8, up: 1, RSRP: -108 dBm, CINR: -3 dB, SNR: -2 dB
I 60.865: Stat: tx 1 kB, rx 0 kB, max 748 B, avg 155 B
I 60.865: Cell updates 3, Network searchs 2 (8 s), PSM delays 0 (0 s), Restarts 0
I 60.865: Wakeups 1, 1 s, connected 11 s, asleep 0 s
OK
```

Each line may starts with a level ('E'rror, 'W'arning, 'I'nfo, or 'D'ebug), which is sometimes usefull for development. If the lines shows a 'b'usy, then the mode is currently executing a command and will reject further commands. After that level a relative time follows with seconds and milliseconds, e.g. "60.856".

That is followed by text, which explains itself a lot. E.g. CAT-M1, roaming, PLMN and so on. 

For energy saving thre function are available for cellular devices:

| Mode | Description | |
| - | :- | :- |
| RAI | Release Assistance Indication | Indicates to switch from RRC active to idle earlier than using a timeout |
| eDRX | Extended Discontinuous Reception | Used in RRC idle mode to reduce power consumption. Usually seconds to a couple of minutes |
| PSM | Power Saving Mode | Enables the device to go from idle into deep sleep. Usually hours or a couple of days |

### rai Command

The device usually tries to request RAI support. If not available, then the timeout is used. Therefore rai is always enabled and if supported, the `net` command shows that in the PSM information.

```
I 60.856: PSM: TAU 90000 [s], Act 0 [s], AS-RAI, Released: 2015 ms
```

In this case, AS-RAI (Access Stratum RAI) is supported from LTE-M network provider.

To disble RAI for tests, you may use 

```
rai off
```

and with 

```
rai on
```

RAI is requested again.

### edrx Command

To request or disable eDRX, use the edrx command:

```
edrx 600
```

requests eDRX with about 600s. Only selected values are supported, the network will assign the next larger value, in this case 655s.

```
I 14.984: eDRX enable, 655.36 s
```

```
edrx off
```

disables eDRX again.

### psm Command

To request or disable PSM, the psm command is used.

```
I 78.096: > help psm:
I 78.096:   psm <act-time> <tau-time>[h] : request PSM times.
I 78.096:      <act-time>    : active time in s.
I 78.096:      <tau-time>    : tracking area update time in s.
I 78.096:      <tau-time>h   : tracking area update time in h.
I 78.096:   psm normal       : PSM handled by application.
```

```
psm 10 48h
```

requests psm with an active timeout of 10s and a tracking are update time of 48 hours or 2 days.

```
I 28.595: PSM enable, act: 10 s, tau: 50 h
...
I 29.265: PSM parameter update: TAU: 180000 s, Active time: 10 s
```

Also here, not all values are available and the next larger one is used. 

```
psm normal
```

Switches the back to the application specific PSM parameters.

** !!! Under Construction !!! **

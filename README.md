![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

# Zephyr - Coaps Demo Client with Eclipse/TinyDtls

## Reliable - Efficient - Encrypted

Version 0.7.0 - September 2023

This [zephyr](https://www.zephyrproject.org/) client demonstrates to use coaps ([CoAP](https://tools.ietf.org/html/rfc7252) over [DTLS 1.2](https://tools.ietf.org/html/rfc6347)) with the [Eclipse/TinyDtls Library](https://github.com/eclipse/tinydtls). In combination with [Eclipse/Californium](https://github.com/eclipse/californium) as Cloud-Server, it enables a device to use [DTLS 1.2 Connection ID](https://tools.ietf.org/html/rfc9146), which obsolete the commonly used frequently DTLS handshakes and eliminates that expensive overhead.
Reducing the messages exchange mostly down to two ip-messages (one request, one response), it enables your device for

- reliable, 
- efficient, and 
- end-to-end encrypted

communication for messages up to a few hundred bytes.
In combination with LTE-M/NB-IoT, CoAP / DTLS 1.2 CID enables to build mobile applications with 

- zero-install and 
- high cost-efficiency.

The demo client itself is in development stage. In "good and normal weather", the `Thingy:91` flies from battery for 7 months. In "storm" it may require to be switched off and on again in very rare exceptions.

The demo client is intended as groundwork for your own ideas. "Out-of-the-box" this application is useful to easily check, if mobile IoT works at the locations and environment you want to use it. The demo reports also some details about the mobile networks functions. 
To build products and applications on this protocol stack requires to implement a production client and to adapt the coap-server for your function. The demo targets therefore people, which are already common with [zephyr](https://www.zephyrproject.org/), or, even better, common with the development for the [Thingy:91](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91) itself.
Without that knowledge it will be more time consuming to make benefit out of this demo.

**Note:**
The demo client is considered to use CoAP/DTLS 1.2 CID. Without server-side support for DTLS 1.2 CID, it will not work proper. Please ensure, that your server supports that.

## Supported Devices

For now, only [nRF9160](https://www.nordicsemi.com/products/nrf9160) based devices are supported.

| Device | |
| :- | - |
| [Nordic Semiconductor, Thingy:91](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91)<br>Works "out-of-the-box" in the "wild". Not easy to extend with custom sensors. | ![Thingy:91](./docu/thingy91.jpg) |
| [Circuit Dojo, nRF9160 feather v5](https://www.jaredwolff.com/store/nrf9160-feather/)<br>Requires additional batteries, antennas, and closures to work in the "wild". The design of the feather allows to easily add custom sensors. | ![nRF9160-feather-v5](https://docs.jaredwolff.com/img/nrf9160-feather-v4-nobg.jpg) |
| [Nordic Semiconductor, nRF9160 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9160-DK)<br>Works "out-of-the-box" on the desk. The design allows to easily add custom sensors. | ![nRF9160-DK](https://www.nordicsemi.com/-/media/Images/Products/DevKits/nRF91-Series/nRF9160-DK.png) |

The demo works with [ncs-2.4.2](https://github.com/nrfconnect/sdk-nrf/tree/v2.4.2).

To benefit from the newer modem features, please consider to use the modem firmware [1.3.5](https://www.nordicsemi.com/-/media/Software-and-other-downloads/Dev-Kits/nRF9160-DK/nRF9160-modem-FW/mfw_nrf9160_1.3.5.zip). See ["Getting started with Thingy:91"](#install-tools-and-tool-chains) below how to apply it.

Maybe other modems and devices gets supported over the time as well. For some of the nRF9160 based devices porting should not be too hard.

| nRF9160 based candidates | |
| :- | - |
| [Sparkfun Thing Plus nRF9160](https://www.sparkfun.com/products/17354?utm_source=sendfox&utm_medium=email&utm_campaign=nrf9160-thing-plus)<br>Qwiic and 2x5 plug for JTAG. | ![Sparkfun Thing Plus nRF9160](https://cdn.sparkfun.com//assets/parts/1/6/3/6/0/17354-SparkFun_Thing_Plus_-_nRF9160-01.jpg) |
| [Icarus IoT Board v2](https://www.actinius.com/icarus)<br>Includes a eSIM.| ![Icarus IoT Board v2](https://www.actinius.com/images/icarus-nrf9160-feather-v2-front-sm-p-500.png) |
| [Conexio Stratus](https://www.crowdsupply.com/conexio/stratus)<br>Built-in energy harvesting capability for autonomous operation. Includes a SIM card with 500MB in 10 years. | ![Conexio Stratus](https://www.crowdsupply.com/img/9c62/side.png) |


## Required HW-Tools for Thingy:91

In order to use this demo with a `Thingy:91`, you need:

- a `Thingy:91` (maybe better two ;-)).
- the `Thingy:91` is usually shipped with a SIM card. Check, if that covers your [area/country](https://www.nordicsemi.com/-/media/Software-and-other-downloads/3rd-party/iBasis-simplified-coverage-map-for-web.pdf). If not, you need a SIM card for your area/country. (Sometimes the `Thingy:91` is shipped with an expired SIM card. Then you will need also an other one.)
- a debug probe to flash the device.
   - either a [nRF9160-DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9160-DK) and a [10-wire-ribbon connector, 2x5, 0.050"](https://www.digikey.com/en/products/detail/harwin-inc/M50-9100542/4953091).
   - or a [Segger j-Link](https://www.segger.com/products/debug-probes/j-link/) and a [cortex-M adapter](https://www.segger.com/products/debug-probes/j-link/accessories/adapters/9-pin-cortex-m-adapter/).

**Note:** the `Thingy:91` uses 1.8V VDD and requires the Jlink to support 1.8V as well. Not all Jlinks supporting that, ensure you get a right one! If the `nRF9160-DK` is used, ensure you select 1.8V for VDD-IO (SW9 on the DK). 

## Required HW-Tools for nRF9160 feather v5

In order to use this demo with a `nRF9160 feather v5`, you need:

- a `nRF9160 feather v5` (maybe better two ;-)).
- the `nRF9160 feather v5` is shipped with a SIM card. Check, if that covers your [area/country](https://www.hologram.io/coverage/). If not, you need a SIM card for your area/country.
- a debug probe to flash the device.
   - either a [nRF9160-DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9160-DK)
   - or a [nRF5340-DK](https://www.nordicsemi.com/Products/Development-hardware/nrf5340-dk)
   (doesn't work for the `Thingy:91`!)
   - or a [Segger j-Link](https://www.segger.com/products/debug-probes/j-link/)
   - and a [TC2030-CTX-NL 6-Pin “No Legs”](https://www.tag-connect.com/product/tc2030-ctx-nl-6-pin-no-legs-cable-with-10-pin-micro-connector-for-cortex-processors) cable with 10-pin micro-connector for Cortex processors for any of the above debug probes.

**Note:** the `nRF9160 feather v5` uses 3.3V VDD and requires the Jlink to support 3.3V as well. Therefore a `nRF5340-DK` can be used. If the `nRF9160-DK` is used, ensure you select 3.0V for VDD-IO (SW9 on the DK). 

## Required HW-Tools for nRF9160-DK

In order to use this demo with a `nRF9160-DK`, you need:

- a `nRF9160-DK` (maybe better two ;-)).
- the `nRF9160-DK` is usually shipped with a SIM card. Check, if that covers your [area/country](https://www.nordicsemi.com/-/media/Software-and-other-downloads/3rd-party/iBasis-simplified-coverage-map-for-web.pdf). If not, you need a SIM card for your area/country. (Sometimes the `nRF9160-DK` is shipped with an expired SIM card. Then you will need also an other one.)
- the `nRF9160-DK` comes with a internal debug probe to flash the device. No additional equipment is required.   

**Note:** the `nRF9160-DK` is a great tool to develop apps for the nRF9160 on your desk. For the "wild", a `Thingy:91` or `nRF9160 feather v5` does a better job.

## Run It - Fast Track

It's not recommended, but you may start with using a pre-build firmware binary. Usually that will take about 1h to send a first message with your `Thingy:91`.

[Fast Track](./docu/FASTTRACK.md)

## Build

In order to be able to build the demo-client, you need to install the development environment. That will take up to an afternoon to send your a message with your `Thingy:91`.

### Install Tools and Tool-Chains

Basically, this requires to follow [Developing with Zephyr](https://docs.zephyrproject.org/latest/develop/index.html).
Though for now only the [Nordic Semiconductor Thingy:91](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91) is supported, it may be easier to go through [Getting started with Thingy:91](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/ug_thingy91_gsg.html). This also required, if you want to update your modem firmware.

Please check the proper installation of your tools building some of the provided samples there (e.g. [zephyr/samples/basic/blinky](https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/basic/blinky) or/and [nrf/samples/nrf9160/udp](https://github.com/nrfconnect/sdk-nrf/tree/main/samples/nrf9160/udp)).

**Note:** both, the zephyr's "developing" and Nordic Semiconductor's "getting started" has changed and may change over time and so it's hard to give good advice. Currently I have good experience with [Nordic Semiconductor - Installing manually](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/gs_installing.html) and Ubuntu 20.04. Installing [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nrf-connect-for-desktop/download#infotabs) and apply the "Toolchain Manager" app works as well on Ubuntu 20.04  (support for 18.04 has been removed for the `nRF Connect for Desktop`). To use this toolchain-manager-installation after installing, start a terminal from that app to get a command console with an setup environment.

![Toolchain Manager](./docu/toolchain-manager.png)

The toolchain-manager-installation also requires a slightly modified sources download, see ["Download the Sources into an available zephyr workspace"](#download-the-sources-into-an-available-zephyr-workspace)

### Download the Sources

This demo comes with a [west.yml](./west.yml) description. Download the demo:

```sh
west init --mr main -m https://github.com/boaks/zephyr-coaps-client.git zephyr-coaps-client
```

> **Note:** 
Please obey the `--mr main`.
Otherwise it will fail fetching the non existing "master" branch!

That creates a `zephyr-coaps-client` folder and you need to populate it further:

```sh
cd zephyr-coaps-client
west update 
```

That takes a while (couple of minutes). It downloads zephyr, the Nordic Semiconductor SDK and the tinydlts zephyr module.

Currently the demo uses the [feature/connection_id](https://github.com/eclipse/tinydtls/tree/feature/connection_id) branch of tinydtls. When that feature branch gets merged into main, this demo will be switched to that.

### Download the Sources into an available zephyr workspace

Using 

```sh
west init --mr main -m https://github.com/boaks/zephyr-coaps-client.git zephyr-coaps-client
```

from a toolchain-manager-installation, fails with an error message, that a workspace already exists.
In order to add just this coaps-demo-app and the tinydtls module library to a workspace, open the workspace (for ncs the "ncs" installation folder and change to "v2.4.2" folder there). Here you find a ".west" folder, that contains the west-configuration for the workspace. Rename that ".west" folder into ".west.org" in order to replace that west-configuration by the one from this example. Now execute 

```sh
west init --mr main -m https://github.com/boaks/zephyr-coaps-client.git
```

That creates a new west-configuration. You need to populate it further:

```sh
west update 
```

That takes a couple of seconds, though zephyr and nrf is already downloaded.

### Build & Flash

After `west` completes the update, build the firmware:
Change the current directory to "zephyr-coaps-client/coaps-client" (or "<workspace>/coaps-client").

```sh
cd coaps-client
west build -b thingy91_nrf9160_ns
```

(For the [nRF9160-DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9160-DK) use `west build -b nrf9160dk_nrf9160_ns`, and for the [nRF9160 feather v5](https://www.jaredwolff.com/store/nrf9160-feather/) use `west build -b circuitdojo_feather_nrf9160_ns`.)

and then flash that resulting firmware to your device

```sh
west flash
```

See also [Updating Firmware Through External Debug Probe](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/ug_thingy91_gsg.html#updating-firmware-through-external-debug-probe).

## Run It

After flashing, the `Thingy:91` starts to blink slow (purple) and after attaching to the mobile-network it switches to green. If the LED is switched off, the device is also connected with the plugtest-server of the [Eclipse/Californium Sandbox](https://github.com/eclipse-californium/californium#interop-server).

**Note:** If the `Thingy:91` starts for the first time in a new area, it may take longer (2-3 minutes) to connect to a mobile network. The `Thingy:91` saves then the configuration and the next time, the startup is much faster, if the `Thingy:91` is not relocated too far.

Press the `Thingy:91`'s call-button (the symbolic "N" in the center of the orange cover). The LED should start with blue and changes very fast to lightblue. If it is re-attached at the mobile-network, it switches to green. And if the response from the server is received, the LED switches off again. On error, the LED is switched to red. Usually it takes all together 1 second (LTE-M/CAT-M1) and only 2 ip-messages are exchanged.

(If you use the [nRF9160-DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9160-DK), then press "Button 1".)

Done.

The demo client exchanges encrypted messages with the coap-server. These messages are only for demonstration, the data is considered to be replaced by your own ideas.

Demo message:

```
4-02:00:11 [d-hh:mm:ss], Thingy:91 v0.7.0+0 (2.4.2), 0*97, 1*1, 2*0, 3*0, failures 0
HW: B1A, MFW: 1.3.5, IMEI: ???????????????
!4127 mV 93% (71 days left) battery
Last code: 2023-09-17T11:32:29Z reboot cmd
ICCID: ????????????????????, eDRX cycle: off, HPPLMN interval: 10 [h]
IMSI: ???????????????
Network: CAT-M1,roaming,Band 20,PLMN 26202,TAC 47490,Cell 52262913,EARFCN 6300
PDN: ???.???,???.??.??.???,rate-limit 256,86400 s
PSM: TAU 90000 [s], Act 0 [s], AS-RAI, Released: 1926 ms
!CE: down: 1, up: 1, RSRP: -119 dBm, CINR: -1 dB, SNR: 0 dB
Stat: tx 83 kB, rx 7 kB, max 862 B, avg 445 B
Cell updates 24, Network searchs 2 (12 s), PSM delays 0 (0 s), Restarts 0
Wakeups 98, 252 s, connected 317 s, asleep 54 s
!34.74 C
!48.00 %H
!973 hPa
```

It starts with the up-time in the first line, followed by the label "Thingy:91" and the client's and NCS version. The sent statistic. "`0*97`" := 97 exchanges without retransmission, "`1*1`" := 1 exchange with 1 retransmission finishs that first line. The current exchange is not included in this statistic. The second line contains the harware version of the nRF9160 chip, the modem firmware version, and the IMEI.
Followed by the line withs the battery status and the lines with information from the SIM-card. In some cases the network details are of interest and the next lines contains that. The last lines of technical informations before the sensor values contains several statistics, e.g. the amount of transfered data and modem restarts.

The demo uses the "echo" resource of the plugtest-server, therefore the response contains just the same message.

If you want to see, what your `Thingy:91` has sent to the server, see [cf-browser](./docu/CFBROWSER.md).

## Configuration

The application comes with a [KConfig](blob/main/Kconfig) to configure some functions. Use

```
west build -b thingy91_nrf9160_ns -t menuconfig
```

for the console variant, and

```
west build -b thingy91_nrf9160_ns -t guiconfig
```

for the GUI variant.

For details please read the provided help for this settings and the [Configuration](./docu/CONFIGURATION.md) page.


## Next Steps

As mentioned at the introduction, the demo is intended as groundwork for your own ideas. 

[Next Steps](./docu/NEXTSTEPS.md)  

See also [Roadmap](./docu/ROADMAP.md) for the plan of the next months.

If you want to consider the power consumption in your idea, please see [Power Consumption](./docu/POWERCONSUMPTION.md) and if you want to make own [measurements](./docu/MEASUREMENTS.md) may be helpful. 

Sometimes it is interesting, which mobile networks are available at some locations. [Thingy:91 - Cellular Explorer](./docu/CELLULAREXPLORER.md) helps here. It comes also with support for a firmware update using XMODEM and some additional function in order to test the features of the mobile network.

A more elaborated example see [mobile-beehive-scale](./docu/MOBILEBEEHIVESCALE.md).

### Updating to a Newer Versions

First update the project itself using 

```sh
cd coaps-client
git pull
```

then update the other modules using

```sh
cd ..
west update
```

In many cases, the next build requires to use 

```sh
cd coaps-client
west build -b thingy91_nrf9160_ns --pristine
```

The `--pristine` resets the current configuration. You may need to configure it again. In some rare cases it may be even required to remove the "build" folder before.

If `west build ... --pristine` keeps failing, the `west update` may require to update also some "west requirements". Therefore execute

```sh
pip3 install --user -r zephyr/scripts/requirements.txt
pip3 install --user -r nrf/scripts/requirements.txt
pip3 install --user -r bootloader/mcuboot/scripts/requirements.txt
```

and retry `west build ... --pristine` again.

## Licenses

This demo itself is licensed under [EPL-2.0](./licenses/EPL-2.0.txt).

Some files are used especially for the `Thingy:91` and are licensed under [Nordic-5](./licenses/Nordic-5.txt). This files are only licensed to be used with Nordic Semiconductor devices.
See [boards](./boards) and [child_image](./child_image).

The demo uses several third-party content, please refer to [NOTICE](./NOTICE.md) for details.

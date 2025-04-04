![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with Eclipse/TinyDtls

# Build Track

In order to be able to build the demo-client, you need to install the development environment. That will take up to an afternoon to send your a message with your `Thingy:91`.

### Install Tools and Tool-Chains

Basically, this requires to follow [Developing with Zephyr](https://docs.zephyrproject.org/latest/develop/index.html).
Though for now only the [Nordic Semiconductor Thingy:91](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91) is supported, it may be easier to go through [Getting started with Thingy:91](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/gsg_guides/thingy91_gsg.html). This is also required, if you want to update your modem firmware.

Please check the proper installation of your tools building some of the provided samples there (e.g. [zephyr/samples/basic/blinky](https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/basic/blinky) or/and [nrf/samples/cellular/udp](https://github.com/nrfconnect/sdk-nrf/tree/main/samples/cellular/udp)).

**Note:** both, the zephyr's "developing" and Nordic Semiconductor's "getting started" has changed and may change over time and so it's hard to give good advice. Previously I had good experience with [Nordic Semiconductor - Installing manually](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/gs_installing.html) on Ubuntu 20.04, but that's in the meantime removed by Nordic. And installing [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nrf-connect-for-desktop/download#infotabs) and applying the "Toolchain Manager" app works as well on Ubuntu 22.04 and Ubuntu 20.04. To use this toolchain-manager-installation after installing, start a terminal from that app to get a command console with an setup environment.

![Toolchain Manager](./toolchain-manager.png)

The toolchain-manager-installation also requires a slightly modified sources download, see ["Download the Sources into an available zephyr workspace"](#download-the-sources-into-an-available-zephyr-workspace)

### Download the Sources

This demo comes with a [west.yml](../west.yml) description. Download the demo:

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

Currently the demo switched back to the [zephyr-cid-develop](https://github.com/boaks/tinydtls/tree/zephyr_cid_develop) branch of my tinydtls fork. This is only intended for temporary convenience because currently many PRs are pending at the eclipse repo and it will take some time to get them merged into "develop" or "main" there. When done, this demo will be switched to that eclipse repo.

### Download the Sources into an available zephyr workspace

Using 

```sh
west init --mr main -m https://github.com/boaks/zephyr-coaps-client.git zephyr-coaps-client
```

from a toolchain-manager-installation, fails with an error message, that a workspace already exists.
In order to add just this coaps-demo-app and the tinydtls module library to a workspace, open the workspace (for ncs the "ncs" installation folder and change to "v2.6.1" folder there). Here you find a ".west" folder, that contains the west-configuration for the workspace. Rename that ".west" folder into ".west.org" in order to replace that west-configuration by the one from this example. Now execute 

```sh
west init --mr main -m https://github.com/boaks/zephyr-coaps-client.git
```

That creates a new west-configuration. You need to populate it further:

```sh
west update 
```

That takes a couple of seconds, though zephyr and nrf is already downloaded.

In some case, if the `zephyr-coaps-client` is already downloaded, also

```sh
west init -l coaps-client
```

may do the job.

### Build & Flash

After `west` completes the update, build the firmware:
Change the current directory to "zephyr-coaps-client/coaps-client" (or "<workspace>/coaps-client").

```sh
cd coaps-client

# NCS 2.9.0
west build -b thingy91/nrf9160/ns

# NCS 2.6.2
west build -b thingy91_nrf9160_ns
```

Other boards are also supported, see table below:

| Board | NCS 2.6.2 | NCS 2.9.0 |
| :- | - | - |
| Thingy:91 | thingy91_nrf9160_ns | thingy91/nrf9160/ns |
| Thingy:91X | thingy91x_nrf9151_ns | thingy91x/nrf9151/ns |
| nRF9160-DK | nrf9160dk_nrf9160_ns | nrf9160dk/nrf9160/ns |
| nRF9161-DK | nrf9161dk_nrf9161_ns | nrf9161dk/nrf9161/ns |
| nRF9151-DK | nrf9151dk_nrf9151_ns | nrf9151dk/nrf9151/ns |
| nRF9160 Feather (v5 and newer) | circuitdojo_feather_nrf9160_ns | circuitdojo_feather/nrf9160/ns |
| nRF9151 Feather | circuitdojo_feather_nrf9151_ns | circuitdojo_feather_nrf9151/nrf9151/ns |
| Conexio Stratus Pro nRF9161 | conexio_stratus_pro_nrf9161_ns | conexio_stratus_pro/nrf9161/ns |
| Conexio Stratus Pro nRF9151 | conexio_stratus_pro_nrf9151_ns | conexio_stratus_pro/nrf9151/ns |
| Makerdiary nRF9151 Connect Kit | -- not supported -- | nrf9151_connectkit/nrf9151/ns |
| MIKROE LTE IoT 4 Click | nRF9160-DK with overlay | nRF9160-DK with overlay |

If the build is successful, the SDK installation was also successful.

**Note:** Starting with v0.10.0 this only builds an image for updates. 

#### Initial Image (with credentials)

For an initial image it's required to provide the used credentials with a project specific config file. Therefore use

```sh
# NCS 2.9.0
west build -b thingy91/nrf9160/ns --pristine -- -DOVERLAY_CONFIG="prov-prj.conf"

# NCS 2.6.2
west build -b thingy91_nrf9160_ns --pristine -- -DOVERLAY_CONFIG="prov-prj.conf"
```

to add the credentials for the [Eclipse/Californium Sandbox](https://github.com/eclipse-californium/californium/blob/main/README.md#interop-server) and then flash that resulting firmware to your device with

```sh
west flash --erase
```

For update images (without "prov-prj.conf") use

```sh
west flash
```

See also [Updating Firmware Through External Debug Probe](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/ug_thingy91_gsg.html#updating-firmware-through-external-debug-probe).

In some case, e.g. a firmware update with an notebook in the wild, it may be easier to use the `nrfjprog` util of the [nRF Command Line Tools](https://www.nordicsemi.com/Products/Development-tools/nrf-command-line-tools/download).

```sh
# NCS 2.9.0
nrfjprog --program build/merged.hex --sectorerase --verify -r

# NCS 2.6.2
nrfjprog --program build/zephyr/merged.hex --sectorerase --verify -r
```

without `west`. If it's the initial image with the device setup, use 

```sh
# NCS 2.9.0
nrfjprog --program build/merged.hex --chiperase --verify -r

# NCS 2.6.2
nrfjprog --program build/zephyr/merged.hex --chiperase --verify -r
```

If you want to protect the firmware from being read via jlink, use

```sh
nrfjprog --rbp all
```

(To use `probe-rs` instead of `nrfjprog` see [../extras/README](../extras/README.md#flashing-new-boards-with-ocd-support).)

## Run It

After flashing, the `Thingy:91` starts to blink slow (purple) and after attaching to the mobile-network it switches to green. If the LED is switched off, the device is also connected with the plugtest-server of the [Eclipse/Californium Sandbox](https://github.com/eclipse-californium/californium#interop-server).

**Note:** If the `Thingy:91` starts for the first time in a new area, it may take longer (2-3 minutes) to connect to a mobile network. The `Thingy:91` saves then the configuration and the next time, the startup is much faster, if the `Thingy:91` is not relocated too far.

Press the `Thingy:91`'s call-button (the symbolic "N" in the center of the orange cover). The LED should start with blue and changes very fast to lightblue. If it is re-attached at the mobile-network, it switches to green. And if the response from the server is received, the LED switches off again. On error, the LED is switched to red. Usually it takes all together 1 second (LTE-M/CAT-M1) and only 2 ip-messages are exchanged.

(If you use the [nRF9160-DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9160-DK), then press "Button 1".)

Done.

The demo client exchanges encrypted messages with the coap-server. These messages are only for demonstration, the data is considered to be replaced by your own ideas.

Demo message:

```
1-01:52:49 [d-hh:mm:ss], Thingy:91 v0.9.101+4, 0*25, 1*1, 2*0, 3*0, failures 0
NCS: 2.5.1, HW: B0A, MFW: 1.3.6, IMEI: 352656100985434
!4154 mV 96% (168 days left) battery
Restart: Reboot
ICCID: 8935711001078444905F, eDRX cycle: off, HPPLMN interval: 10 [h]
IMSI: 278773000008810
Network: CAT-M1,roaming,Band 20,PLMN 26202,TAC 47490,Cell 52470017,EARFCN 6300
PDN: flolive.net,100.64.153.239,rate-limit 256,86400 s
PSM: TAU 90000 [s], Act 0 [s], AS-RAI, Released: 2021 ms
!CE: down: 1, up: 1, RSRP: -108 dBm, CINR: 0 dB, SNR: 1 dB
Stat: tx 21 kB, rx 2 kB, max 872 B, avg 401 B
Cell updates 17, Network searchs 3 (326 s), PSM delays 0 (0 s)
Modem Restarts 0, Sockets 3, DTLS handshakes 1
Wakeups 25, 93 s, connected 150 s, asleep 0 s
!22.81 C
!46.36 %H
!1002 hPa
```

It starts with the up-time in the first line, followed by the label "Thingy:91" and the client's and NCS version. The sent statistic. "`0*25`" := 25 exchanges without retransmission, "`1*1`" := 1 exchange with 1 retransmission finishs that first line. The current exchange is not included in this statistic. The second line contains the harware version of the nRF9160 chip, the modem firmware version, and the IMEI.
Followed by the line withs the battery status and the lines with information from the SIM-card. In some cases the network details are of interest and the next lines contains that. The last lines of technical informations before the sensor values contains several statistics, e.g. the amount of transfered data and modem restarts.

The demo uses the "echo" resource of the plugtest-server, therefore the response contains just the same message.

If you want to see, what your `Thingy:91` has sent to the server, see [cf-browser](./CFBROWSER.md).

## Settings And Provisioning

Starting with version 0.10.0 the [SETTINGS](https://docs.zephyrproject.org/latest/services/settings/index.html) service is used to store device specific data as credentials. Without that using different credentials in the firmware per device would cause to use also an firmware image per device. Therefore this projects provides now two variants of images:

- image with initial settings. To be applied at "end of line", means once after production. 
- image without settings. To be applied for firmware updates, e.g. FOTA.

The initial image will only apply the settings on the first start after flashing it with `--chiperase`

```sh
# NCS 2.9.0
nrfjprog --program build/merged.hex --chiperase --verify -r

# NCS 2.6.2
nrfjprog --program build/zephyr/merged.hex --chiperase --verify -r
```

When this image is applied afterwards with `--sectorerase`

```sh
# NCS 2.9.0
nrfjprog --program build/merged.hex --sectorerase --verify -r

# NCS 2.6.2
nrfjprog --program build/zephyr/merged.hex --sectorerase --verify -r
```

then the settings are unchanged.

The update image must not be applied with `--chiperase`, it must always be applied after an initial image and as any update with `--sectorerase`.

The solves only a part of the issue with the credentials. If the credentials are included in the initial image, this would require to build images for each device with different credentials. Otherwise it would be required to set the credentials after the inital image is applied. That is also possible using the [sh-cmds](./SHCMDSLIST.md) via a terminal, but is not that comfortable. To optimize this process, the firmware has an additional "auto-provisioning" feature. With that, the device creates either a `Raw Public Key ECDSA key pair` (preferred) or/and a random `PSK secret`. It uses then the included auto-provisioning-credentials and executes an provisioning request. After success, the device switches to the generated credentials and executes a new handshake. Ready to send data to the cloud server.

[prov-prj.conf](../prov-prj.conf) to see the configuration value, which are supported by the settings service. 

## Credentials

Starting with version 0.10.0 ECDSA certificates are also supported. Not x509 certificates but the slim **R**aw **P**ublic **K**ey variant (see [RFC 7250](https://www.rfc-editor.org/rfc/rfc7250)). This requires also support on ther server-side (e.g. as with Californium).

The RPK certificates are intended to be provisioned ahead. Usually a key pair is created with

```sh
openssl ecparam -genkey -name prime256v1 -noout -out privkey.pem
```

or

```sh
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out privkey.pem
```

You will need one key pair for the server, and one key pair for each device. To make the device and server trust each other, they need to exchange their `public key`s ahead. Therefore extract the `public key` from the key pair using

```sh
openssl pkey -pubout -in privkey.pem 

-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAENTGXGkhc7gL614R4HBOkXoESM98Y
IXP3yts4VG7wpRlsIxYFFXVez3I3VE7oGaOpLlAMMhFa4Myq/4OIRMvauQ==
-----END PUBLIC KEY-----
```

That prints the `public key` in ASN.1 (including the algorithm information) in base 64 encoding. For the device side, copy the base 64 part of the server's `public key` into a single line and paste that in the `CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY` value of the [prov-prj.conf](../prov-prj.conf) file.

```
CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY="MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAENTGXGkhc7gL614R4HBOkXoESM98YIXP3yts4VG7wpRlsIxYFFXVez3I3VE7oGaOpLlAMMhFa4Myq/4OIRMvauQ=="
```

The device also requires it's own `key pair`, therefore extract the `private key` with

```sh
openssl ec -no_public -in privkey.pem 

read EC key
writing EC key
-----BEGIN EC PRIVATE KEY-----
MDECAQEEIMjsiXRzR3OYtELs+9tWYHB4/nT9x3LAXFzA8ezR8iVLoAoGCCqGSM49
AwEH
-----END EC PRIVATE KEY-----
```

and copy again the base 64 part as single line into the `CONFIG_DTLS_ECDSA_PRIVATE_KEY` value of the [prov-prj.conf](../prov-prj.conf) file.

(**Note**: the output format of `openssl ec -no_public` is different from the one used for v0.10.0. Please ensure, you're using the current sources of v0.10.1, which supports also this openssl encoding.)

```
CONFIG_DTLS_ECDSA_PRIVATE_KEY="MDECAQEEIMjsiXRzR3OYtELs+9tWYHB4/nT9x3LAXFzA8ezR8iVLoAoGCCqGSM49AwEH"
```

You will not need to provide the device's `public key`, because that is calculated from the provided `private key` by the device itself.

If you use the [cf-cloud-demo-server](https://github.com/eclipse-californium/californium/tree/main/demo-apps/cf-cloud-demo-server) or the [cf-s3-proxy-server](https://github.com/eclipse-californium/californium/tree/main/demo-apps/cf-s3-proxy-server) [Device Credentials](https://github.com/eclipse-californium/californium/tree/main/demo-apps/cf-cloud-demo-server#device-credentials) shows how to add the keys to the server side.

### Credentials Auto Provisioning

Though the above process requires a couple of manual steps, there is also an automated way to provide the device credentials to the server.

This approach uses a special `auto-provisioning key pair` on the device to provide the generated credentials to the server. Create a `key pair` as above and extract the `private key` as also explained above. Instead of copy the base 64 part to the `CONFIG_DTLS_ECDSA_PRIVATE_KEY` value, copy it to the `CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY` value of the [prov-prj.conf](../prov-prj.conf) file.

```
CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY="MDECAQEEIMjsiXRzR3OYtELs+9tWYHB4/nT9x3LAXFzA8ezR8iVLoAoGCCqGSM49AwEH"
```

Some onfiguration values must also be applied additionaliy, so apply:

```
# ECDSA credentials
# ascii in 'ascii', hexadecimal with prefix ":0x", or base64
CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY="<server's public key in base 64>"
CONFIG_DTLS_ECDSA_PRIVATE_KEY_GENERATE=y
# ascii in 'ascii', hexadecimal with prefix ":0x", or base64
CONFIG_DTLS_ECDSA_PRIVATE_KEY=""

# Auto provisioning
CONFIG_PROVISIONING_GROUP="Demo"
CONFIG_DTLS_ECDSA_AUTO_PROVISIONING=y
# ascii in 'ascii', hexadecimal with prefix ":0x", or base64
CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY="MDECAQEEIMjsiXRzR3OYtELs+9tWYHB4/nT9x3LAXFzA8ezR8iVLoAoGCCqGSM49AwEH"
```

to that [prov-prj.conf](../prov-prj.conf) file.

- fill the server's `public key` in the field `CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY`
- set `CONFIG_DTLS_ECDSA_PRIVATE_KEY_GENERATE` to `y`
- clear `CONFIG_DTLS_ECDSA_PRIVATE_KEY`
- set `CONFIG_DTLS_ECDSA_AUTO_PROVISIONING` to `y`
- fill the `auto-provisioning private key` in the field `CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY`

If you use the [cf-cloud-demo-server](https://github.com/eclipse-californium/californium/tree/main/demo-apps/cf-cloud-demo-server) or the [cf-s3-proxy-server](https://github.com/eclipse-californium/californium/tree/main/demo-apps/cf-s3-proxy-server) create a device entry with `public key` of the `auto-provisioning private key` and add the field `.prov=1` to indicate the "auto-provisioning" function for that entry.

```
Provisioning1=Admin
.rpk=MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAENTGXGkhc7gL614R4HBOkXoESM98YIXP3yts4VG7wpRlsIxYFFXVez3I3VE7oGaOpLlAMMhFa4Myq/4OIRMvauQ==
.prov=1
```

You may now apply the same resulting "..._full.hex" image to a set of devices. Each will generate it's own `key pair` and will provide that to the server using the same `auto-provisioning key-pair`. After that, the devices starts their communication using their own generated `key pair`. With the next update, the `auto-provisioning key-pair` will be removed from the device's application flash. 

**Note:** though the `auto-provisioning key-pair` is shared between a set of devices, it's recommended to exchange it frequently. That will cause devices, which are using old `auto-provisioning key-pair` for late "auto-provisioning" to fail. Though the intention is to execute the "auto-provisioning" at the "end-of-line" (production), that should not be an issue.  

## Configuration

The application comes with a [KConfig](../Kconfig) to configure some functions. Use

```
# NCS 2.9.0
west build -b thingy91/nrf9160/ns -t menuconfig

# NCS 2.6.2
west build -b thingy91_nrf9160_ns -t menuconfig
```

for the console variant, and

```
# NCS 2.9.0
west build -b thingy91/nrf9160/ns -t guiconfig

# NCS 2.6.2
west build -b thingy91_nrf9160_ns -t guiconfig
```

for the GUI variant.

For details please read the provided help for this settings and the [Configuration](./CONFIGURATION.md) page.

## Next Steps

As mentioned at the introduction, the demo is intended as groundwork for your own ideas. 

[Next Steps](./NEXTSTEPS.md)  

See also [Roadmap](./ROADMAP.md) for the plan of the next months.

If you want to consider the power consumption in your idea, please see [Power Consumption](./POWERCONSUMPTION.md) and if you want to make own [measurements](./MEASUREMENTS.md) may be helpful. 

Sometimes it is interesting, which mobile networks are available at some locations. [Thingy:91 - Cellular Explorer](./CELLULAREXPLORER.md) helps here. It comes also with support for a firmware update using XMODEM and some additional function in order to test the features of the mobile network.

A more elaborated example see [mobile-beehive-scale](./MOBILEBEEHIVESCALE.md).

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

# NCS 2.9.0
west build -b thingy91/nrf9160/ns --pristine

# NCS 2.6.2
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

### Manually Update the Zephyr SDK 

In some case it may be required to also update the [Zephyr SDK](
https://docs.zephyrproject.org/latest/develop/getting_started/index.html#install-the-zephyr-sdk). Follow the instructions there and test, if that fixes your issues when updating the NCS version. 


![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

# Zephyr - Coaps Demo Client with Eclipse/TinyDtls

## Prebuild binaries v0.12.0

This folder contains prebuild binaries intended to be used to get a first impression of CoAP / DTLS 1.2 CID. The compiled in destination server is californium.eclipseprojects.io. To gather more experience, please build and modify this example on your own.

Starting with 0.10.0, the "..._full.hex" binaries are required to be applied first using Jlink. That's required to initialize the new introduced settings service. Afterwards the update binaries may be used to update the device with new functions. 

Also starting with 0.10.0, the prebuild binaries doesn't longer support updates via the serial bootloader interface. It's still supported using XMODEM or FOTA. For custom builds it may be reenabled using [mcuboot.conf](../../../raw/main/child_image/mcuboot.conf). 

For each supported board, 3 images are available:

| File Pattern | Description |
| - | - |
| \*_full.hex | `full image` including mcu_boot, application and settings. Applied with debug probe. Since 0.10.0 required as initial step. |
| \*_app_update.hex | `update image`, only application. Applied with debug probe. |
| \*_app_update.bin | `update image`, only application. Applied with FOTA (CoAP) or serial (XMODEM) |

Using linux, please ensure, that the [permissions are granted](../extras/udev-rules/README.md).
For Windows you will need to install the drivers for the USB interfaces.

## License

Please refer to [NOTICE](../NOTICE.md).

[EPL-2.0 - extraction](https://www.eclipse.org/legal/epl-2.0/)


> 5. NO WARRANTY
> 
> EXCEPT AS EXPRESSLY SET FORTH IN THIS AGREEMENT, AND TO THE EXTENT PERMITTED BY APPLICABLE LAW, THE PROGRAM IS PROVIDED ON AN “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED INCLUDING, WITHOUT LIMITATION, ANY WARRANTIES OR CONDITIONS OF TITLE, NON-INFRINGEMENT, MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Each Recipient is solely responsible for determining the appropriateness of using and distributing the Program and assumes all risks associated with its exercise of rights under this Agreement, including but not limited to the risks and costs of program errors, compliance with applicable laws, damage to or loss of data, programs or equipment, and unavailability or interruption of operations.
> 6. DISCLAIMER OF LIABILITY
> 
> EXCEPT AS EXPRESSLY SET FORTH IN THIS AGREEMENT, AND TO THE EXTENT PERMITTED BY APPLICABLE LAW, NEITHER RECIPIENT NOR ANY CONTRIBUTORS SHALL HAVE ANY LIABILITY FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING WITHOUT LIMITATION LOST PROFITS), HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OR DISTRIBUTION OF THE PROGRAM OR THE EXERCISE OF ANY RIGHTS GRANTED HEREUNDER, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGES. 

Therefore, before you start, ensure, that you're common with the tools in order to reset you device back in the state before you apply this firmware.

## [nrf9160 feather](https://www.circuitdojo.com/products/nrf9160-feather)

- [full image: circuitdojo_feather_full.hex](../../../raw/main/prebuild/circuitdojo_feather_full.hex)
- [update image: circuitdojo_feather_app_update.hex](../../../raw/main/prebuild/circuitdojo_feather_app_update.hex)
- [update image: circuitdojo_feather_app_update.bin](../../../raw/main/prebuild/circuitdojo_feather_app_update.bin)

If the serial bootloader interface is enabled again for custombuilds, the "..._app_update.hex" may be used for updates without debug probe via USB and [bootloader with newtmgr](https://docs.jaredwolff.com/nrf9160-programming-and-debugging.html#bootloader-use). You need to ensure also, that "\~/.zephyrtools/newtmgr/" and "\~/.zephyrtools/zephyr-tools/" are added to your `PATH`.

In difference to the original mcuboot loader of the feather, which uses a baudrate of 1000000, the contained bootloader uses a baudrate of 115200, in line with the baudrate of the later logging and sh-communication.
 
For automatic bootloader support on linux, please ensure, that the [permissions are granted](../extras/udev-rules/README.md)

For Windows you still need to switch on the bootloader mode manually by pressing the mode button during reset. 

## [nrf9151 feather](https://www.circuitdojo.com/products/nrf9151-feather)

- [full image: circuitdojo_feather_nrf9151_full.hex](../../../raw/main/prebuild/circuitdojo_feather_nrf9151_full.hex)
- [update image: circuitdojo_feather_nrf9151_app_update.hex](../../../raw/main/prebuild/circuitdojo_feather_nrf9151_app_update.hex)
- [update image: circuitdojo_feather_nrf9151_app_update.bin](../../../raw/main/prebuild/circuitdojo_feather_nrf9151_app_update.bin)

Requires [probe-rs](../extras/README.md#flashing-new-boards-with-ocd-support) to apply an image with the onboard debug probe. 

## [Conexio Stratus Pro nrf9151](https://conexiotech.com/conexio-stratus-pro-nrf9151/)

- [full image: conexio_stratus_pro_full.hex](../../../raw/main/prebuild/conexio_stratus_pro_full.hex)
- [update image: conexio_stratus_pro_app_update.hex](../../../raw/main/prebuild/conexio_stratus_pro_app_update.hex)
- [update image: conexio_stratus_pro_app_update.bin](../../../raw/main/prebuild/conexio_stratus_pro_app_update.bin)

## [nrf9160 Thingy:91](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91)

- [full image: thingy91_full.hex](../../../raw/main/prebuild/thingy91_full.hex)
- [update image: thingy91_app_update.hex](../../../raw/main/prebuild/thingy91_app_update.hex)
- [update image: thingy91_app_update.bin](../../../raw/main/prebuild/thingy91_app_update.bin)

If the serial bootloader interface is enabled again for custombuilds, the "..._app_update.hex" may be used without debug probe via USB and [nRF Connect Programmer](https://docs.nordicsemi.com/bundle/nrf-connect-programmer/page/index.html).

## [nrf9151 Thingy:91X](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91-X)

- [full image: thingy91x_full.hex](../../../raw/main/prebuild/thingy91x_full.hex)
- [update image: thingy91x_app_update.hex](../../../raw/main/prebuild/thingy91x_app_update.hex)
- [update image: thingy91x_app_update.bin](../../../raw/main/prebuild/thingy91x_app_update.bin)

## [nrf9160 DevKit](https://www.nordicsemi.com/Products/Development-hardware/nrf9160-dk)

- [full image: nrf9160dk_full.hex](../../../raw/main/prebuild/nrf9160dk_full.hex)
- [update image: nrf9160dk_app_update.hex](../../../raw/main/prebuild/nrf9160dk_app_update.hex)
- [update image: nrf9160dk_app_update.bin](../../../raw/main/prebuild/nrf9160dk_app_update.bin)

## [nrf9151 DevKit](https://www.nordicsemi.com/Products/Development-hardware/nrf9151-dk)

- [full image: nrf9151dk_full.hex](../../../raw/main/prebuild/nrf9151dk_full.hex)
- [update image: nrf9151dk_app_update.hex](../../../raw/main/prebuild/nrf9151dk_app_update.hex)
- [update image: nrf9151dk_app_update.bin](../../../raw/main/prebuild/nrf9151dk_app_update.bin)


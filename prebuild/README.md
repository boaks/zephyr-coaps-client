![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

# Zephyr - Coaps Demo Client with Eclipse/TinyDtls

## Prebuild binaries v0.8.0

This folder contains 8 prebuild binaries intended to be used to get a first impression of CoAP / DTLS 1.2 CID. The compiled in destination server is californium.eclipseprojects.io. To gather more experience, please build and modify this example on your own.

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

## [nrf9160 feather](https://www.jaredwolff.com/store/nrf9160-feather/)

- [full image: circuitdojo_feather_nrf9160_ns_full.hex](../../../raw/main/prebuild/circuitdojo_feather_nrf9160_ns_full.hex) using a debug probe, this is the preferred approach
- [signed app image: circuitdojo_feather_nrf9160_ns_app_signed.hex](../../../raw/main/prebuild/circuitdojo_feather_nrf9160_ns_app_signed.hex) app signed with the demo keys. May be used without debug probe via USB and [bootloader with newtmgr](https://docs.jaredwolff.com/nrf9160-programming-and-debugging.html#bootloader-use). You need to ensure also, that "\~/.zephyrtools/newtmgr/" and "\~/.zephyrtools/zephyr-tools/" are added to your `PATH`. 
- [signed app update: circuitdojo_feather_nrf9160_ns_app_update.bin](../../../raw/main/prebuild/circuitdojo_feather_nrf9160_ns_app_update.bin) app signed with the demo keys. May be used for FOTA (CoAP) or XMODEM updates.

For automatic bootloader support on linux, please ensure, that the permissions are granted by

   SUBSYSTEM=="usb", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60" GROUP="users", MODE="0666"

in a file e.g. "50-nrf9160-feather.rules" and put in "/etc/udev/rules.d".

For Windows you still need to switch on the bootloader mode manually by pressing the mode button during reset. 

## [nrf9160 Thingy:91](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91)

- [full image: thingy91_nrf9160_ns_full.hex](../../../raw/main/prebuild/thingy91_nrf9160_ns_full.hex) using a debug probe, this is the preferred approach
- [signed app image: thingy91_nrf9160_ns_app_signed.hex](../../../raw/main/prebuild/thingy91_nrf9160_ns_app_signed.hex) app signed with the demo keys. May be used without debug probe via USB and [nRF Connect Programmer](https://infocenter.nordicsemi.com/index.jsp?topic=/struct_nrftools/struct/nrftools_nrfconnect.html).
- [signed app update: thingy91_nrf9160_ns_app_update.bin](../../../raw/main/prebuild/thingy91_nrf9160_ns_app_update.bin) app signed with the demo keys. May be used for FOTA (CoAP) or XMODEM updates.

## [nrf9160 DevKit](https://www.nordicsemi.com/Products/Development-hardware/nrf9160-dk)

- [full image: nrf9160dk_nrf9160_ns_full.hex](../../../raw/main/prebuild/nrf9160dk_nrf9160_ns_full.hex) using the onboard debug probe of the devkit.
- [signed app update: nrf9160dk_nrf9160_ns_app_update.bin](../../../raw/main/prebuild/nrf9160dk_nrf9160_ns_app_update.bin) app signed with the demo keys. May be used for FOTA (CoAP) or XMODEM updates.

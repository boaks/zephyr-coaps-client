![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

# Fast Track

If you only want to use for testing cellular coverage, you may start with using the [pre-build firmware images](../prebuild/). 

## Install 

Download the [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-desktop) and start it. You will need the `Programmer` App. If not already installed, please install it using the button in the nRF Connect for Desktop application.

Download one of the [pre-build firmware images](../prebuild/) matching your device and available tools.

Connect the `Thingy:91` to your debug probe and use the [nRF Connect Programmer](https://docs.nordicsemi.com/bundle/nrf-connect-programmer/page/index.html) to apply the downloaded pre-build firmware.

Using [nRF Connect Programmer](https://docs.nordicsemi.com/bundle/nrf-connect-programmer/page/index.html) requires, that the device is detected as "genuine" Nordic devices. That unfortunately doesn't support to use other USB controllers. If a JLink probe is used, that target must be a "genuine" Nordic device and must be already connected starting the programmer app. Otherwise you may get an error message as

    Unsupported device. The detected device could not be recognized as neither JLink device nor Nordic USB device.

## Run It

Go to [Build Track](./BUILDTRACK.md#run-it).

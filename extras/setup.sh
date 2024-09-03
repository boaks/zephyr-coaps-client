#!/bin/sh

cp -r circuitdojo_feather_nrf9161 ../../zephyr/boards/arm/
cp mcuboot/circuitdojo_feather_nrf9161.conf ../../bootloader/mcuboot/boot/zephyr/boards

cp -r conexio_stratus_pro ../../zephyr/boards/arm/
cp mcuboot/conexio_stratus_pro.conf ../../bootloader/mcuboot/boot/zephyr/boards


#!/bin/sh

# only for 2.6.x! 

cp -r v2.6.x/circuitdojo_feather_nrf9161 ../../zephyr/boards/arm/
cp v2.6.x/mcuboot/circuitdojo_feather_nrf9161.conf ../../bootloader/mcuboot/boot/zephyr/boards

cp -r v2.6.x/conexio_stratus_pro ../../zephyr/boards/arm/
cp v2.6.x/mcuboot/conexio_stratus_pro_nrf9151.conf ../../bootloader/mcuboot/boot/zephyr/boards
cp v2.6.x/mcuboot/conexio_stratus_pro_nrf9161.conf ../../bootloader/mcuboot/boot/zephyr/boards


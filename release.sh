#!/bin/sh

date

west build -d build_nrf9160dk_nrf9160_ns -b nrf9160dk_nrf9160_ns --pristine

west build -d build_thingy91_nrf9160_ns -b thingy91_nrf9160_ns --pristine -- -DOVERLAY_CONFIG="60min0-prj.conf"

west build -d build_feather_nrf9160_ns -b circuitdojo_feather_nrf9160_ns --pristine -- -DOVERLAY_CONFIG="60min0-prj.conf"

west build -d build_nrf9160dk_nrf9160_ns -b nrf9160dk_nrf9160_ns -- -DCOPY_PREBUILDS=On

west build -d build_thingy91_nrf9160_ns -b thingy91_nrf9160_ns -- -DOVERLAY_CONFIG="60min0-prj.conf" -DCOPY_PREBUILDS=On

west build -d build_feather_nrf9160_ns -b circuitdojo_feather_nrf9160_ns -- -DOVERLAY_CONFIG="60min0-prj.conf" -DCOPY_PREBUILDS=On


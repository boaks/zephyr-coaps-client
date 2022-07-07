#
# Copyright (c) 2022 Achim Kraus CloudCoap.net
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Eclipse Public License 2.0 which is available at
# http://www.eclipse.org/legal/epl-2.0
#
# SPDX-License-Identifier: EPL-2.0
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(coaps-client)

# APP START
target_sources(app PRIVATE 
	src/main.c 
	src/modem.c 
	src/power_manager.c 
	src/ui.c 
	src/dtls_client.c 
	src/dtls_credentials.c 
	src/coap_client.c)

target_sources_ifdef(CONFIG_ADXL362_MOTION_DETECTION app PRIVATE src/accelerometer_sensor.c)

target_sources_ifdef(CONFIG_LOCATION_ENABLE app PRIVATE src/modem_location.c)

target_sources_ifdef(CONFIG_BME680 app PRIVATE src/environment_sensor.c)

if (CONFIG_BME680_BSEC)
        set(bsec_version "BSEC_1.4.8.0_Generic_Release_updated_v3")
        set(bsec_dir "${ZEPHYR_NRF_MODULE_DIR}/ext/${bsec_version}")

        if (CONFIG_FP_SOFTABI)
                set(BSEC_LIB_DIR ${bsec_dir}/algo/normal_version/bin/gcc/Cortex_M33)
        endif()
        if (CONFIG_FP_HARDABI)
                set(BSEC_LIB_DIR ${bsec_dir}/algo/normal_version/bin/gcc/Cortex_M33F)
        endif()

        if(NOT EXISTS "${BSEC_LIB_DIR}/libalgobsec.a")
                assert(0 "Could not find BSEC library in \"${bsec_dir}\".")
        endif()

        if (CONFIG_BME680_BSEC_SAMPLE_MODE_ULTRA_LOW_POWER)
                set(BSEC_CONFIG_DIR ${bsec_dir}/config/generic_18v_300s_4d)
        endif()
        if (CONFIG_BME680_BSEC_SAMPLE_MODE_LOW_POWER)
                set(BSEC_CONFIG_DIR ${bsec_dir}/config/generic_18v_3s_4d)
        endif()
        set(BSEC_EXAMPLE_DIR ${bsec_dir}/examples/bsec_iot_example)

        target_include_directories(app PRIVATE ${BSEC_LIB_DIR} ${BSEC_CONFIG_DIR} ${BSEC_EXAMPLE_DIR})

        target_sources(app PRIVATE 
                src/environment_sensor.c
                ${BSEC_CONFIG_DIR}/bsec_serialized_configurations_iaq.c
                ${bsec_dir}/examples/bsec_iot_example/bsec_integration.c
                ${bsec_dir}/examples/bsec_iot_example/bme680.c)

        add_library(bsec_lib STATIC IMPORTED GLOBAL)
        add_dependencies(bsec_lib math_lib bsec_target)

        set_target_properties(bsec_lib PROPERTIES IMPORTED_LOCATION "${BSEC_LIB_DIR}/libalgobsec.a")
        set_target_properties(bsec_lib PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${BSEC_LIB_DIR}")

        target_link_libraries(bsec_lib INTERFACE -L${LIBC_LIBRARY_DIR})
        target_link_libraries(app PUBLIC bsec_lib)
endif()


zephyr_include_directories(src)

# tinydtls - support DTLS 1.2 Connection ID
zephyr_library_link_libraries(tinydtls)

# APP END

if (COPY_PREBUILDS)
   # enable west build -b ... -- -DCOPY_PREBUILDS=On
   message(NOTICE "copy prebuild binaries into ${CMAKE_BINARY_DIR}/../prebuild")
   file(COPY_FILE ${CMAKE_BINARY_DIR}/zephyr/merged.hex ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_full.hex)
   file(COPY_FILE ${CMAKE_BINARY_DIR}/zephyr/app_signed.hex ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_app_signed.hex)
endif()

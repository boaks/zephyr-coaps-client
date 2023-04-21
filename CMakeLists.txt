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
	src/parse.c
	src/transform.c
	src/power_manager.c
	src/io_job_queue.c
	src/ui.c
	src/appl_eeprom.c
	src/appl_time.c
	src/modem.c
	src/dtls_client.c
	src/dtls_credentials.c
	src/coap_client.c
	)

target_sources_ifdef(CONFIG_CONSOLE_SUBSYS app PRIVATE src/console_input.c)

target_sources_ifdef(CONFIG_LOCATION_ENABLE app PRIVATE src/location.c)

target_sources_ifdef(CONFIG_MOTION_SENSOR app PRIVATE src/accelerometer_sensor.c)

target_sources_ifdef(CONFIG_BME680 app PRIVATE src/environment_sensor.c src/environment_history.c)

target_sources_ifdef(CONFIG_SHT3XD app PRIVATE src/environment_sensor.c src/environment_history.c)

target_sources_ifdef(CONFIG_SHT21 app PRIVATE src/environment_sht21.c src/environment_history.c)

target_sources_ifdef(CONFIG_BATTERY_ADC app PRIVATE src/battery_adc.c)

if (CONFIG_BME680_BSEC)
        set(bsec_version "bsec_1-4-9-2_generic_release")
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
        set(BSEC_EXAMPLE_DIR ${bsec_dir}/examples)

        target_include_directories(app PRIVATE ${BSEC_LIB_DIR} ${BSEC_CONFIG_DIR} ${BSEC_EXAMPLE_DIR})

        target_sources(app PRIVATE 
                src/environment_sensor.c
                 src/environment_history.c
                ${BSEC_CONFIG_DIR}/bsec_serialized_configurations_iaq.c
                ${BSEC_EXAMPLE_DIR}/bsec_integration.c
                ${BSEC_EXAMPLE_DIR}/bme68x.c)

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
   # Enable west build -b ... -- -DCOPY_PREBUILDS=On
   # Copies the .hex files before the build!
   # Therefore build it first without,
   # and a second time with -DCOPY_PREBUILDS=On

   message(NOTICE "copy prebuild binaries into ${CMAKE_BINARY_DIR}/../prebuild")
   file(COPY_FILE ${CMAKE_BINARY_DIR}/zephyr/merged.hex ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_full.hex)
   file(COPY_FILE ${CMAKE_BINARY_DIR}/zephyr/app_signed.hex ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_app_signed.hex)
endif()

if (CONFIG_COAP_SEND_INTERVAL)
set(BUILD_SUMUP1 "Build with send interval ${CONFIG_COAP_SEND_INTERVAL} s.")
else()
set(BUILD_SUMUP1 "Build without send interval.")
endif()

if (CONFIG_LOCATION_ENABLE)
set(BUILD_SUMUP2 "Build with GNSS.")
else()
set(BUILD_SUMUP2 "Build without GNSS.")
endif()

if (CONFIG_BME680)
set(BUILD_SUMUP3 "Build with BME680.")
elseif (CONFIG_BME680_BSEC)
set(BUILD_SUMUP3 "Build with BME680 BSEC.")
elseif (CONFIG_SHT3XD)
set(BUILD_SUMUP3 "Build with SHT31.")
elseif (CONFIG_SHT21)
set(BUILD_SUMUP3 "Build with SHT21.")
else()
set(BUILD_SUMUP3 "Build without BME680/SHT21/SHT31.")
endif()

if (CONFIG_BME680 OR CONFIG_BME680_BSEC OR CONFIG_SHT3XD OR CONFIG_SHT21)
if (CONFIG_ENVIRONMENT_HISTORY_SIZE GREATER 0)
set(BUILD_SUMUP4 "Build with history using ${CONFIG_ENVIRONMENT_HISTORY_SIZE} values.")
else()
set(BUILD_SUMUP4 "Build without history.")
endif()
else()
set(BUILD_SUMUP4 "Build without history, no BME680/SHT21/SHT31.")
endif()


if (CONFIG_COAP_WAIT_ON_POWERMANAGER)
set(BUILD_SUMUP5 "Build with waiting for power-manager.")
else()
set(BUILD_SUMUP5 "Build without waiting for power-manager.")
endif()

if (CONFIG_SUSPEND_3V3 AND CONFIG_SUSPEND_UART)
set(BUILD_SUMUP6 "Suspend 3.3V and UART.")
elseif (CONFIG_SUSPEND_3V3)
set(BUILD_SUMUP6 "Suspend 3.3V.")
elseif (CONFIG_SUSPEND_UART)
set(BUILD_SUMUP6 "Suspend UART.")
else()
set(BUILD_SUMUP6 "Not suspending 3.3V nor UART.")
endif()

if (CONFIG_BATTERY_VOLTAGE_SOURCE_ADP536X)
set(BUILD_SUMUP7 "Build with ADP536X battery voltage.")
elseif (CONFIG_BATTERY_VOLTAGE_SOURCE_ADC)
set(BUILD_SUMUP7 "Build with ADC battery voltage.")
else()
set(BUILD_SUMUP7 "Build with modem battery voltage.")
endif()

if (CONFIG_BATTERY_TYPE_LIPO_1350_MAH)
set(BUILD_SUMUP8 "Build for 1350 mAh LiPo")
elseif (CONFIG_BATTERY_TYPE_LIPO_2000_MAH)
set(BUILD_SUMUP8 "Build for 2000 mAh LiPo")
elseif (CONFIG_BATTERY_TYPE_ENELOOP_2000_MAH)
set(BUILD_SUMUP8 "Build for 2000 mAh Eneloop")
else()
set(BUILD_SUMUP8 "Build without battery.")
endif()

add_custom_command(TARGET app POST_BUILD
                   COMMAND echo "${BUILD_SUMUP1}"
                   COMMAND echo "${BUILD_SUMUP2}"
                   COMMAND echo "${BUILD_SUMUP3}"
                   COMMAND echo "${BUILD_SUMUP4}"
                   COMMAND echo "${BUILD_SUMUP5}"
                   COMMAND echo "${BUILD_SUMUP6}"
                   COMMAND echo "${BUILD_SUMUP7}"
                   COMMAND echo "${BUILD_SUMUP8}"
                   COMMAND echo "Build with network search timeout ${CONFIG_MODEM_SEARCH_TIMEOUT}s"
                   )

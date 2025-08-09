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

cmake_minimum_required(VERSION 3.29.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(coaps-client)

target_compile_options(app PRIVATE -fanalyzer)

# APP START
target_sources(app PRIVATE
	src/parse.c
	src/transform.c
	src/power_manager.c
	src/io_job_queue.c
	src/ui.c
	src/appl_diagnose.c
	src/appl_settings.c
	src/appl_time.c
	src/modem.c
	src/modem_desc.c
	src/modem_sim.c
	src/modem_at.c
	src/dtls_client.c
	src/coap_client.c
	src/coap_appl_client.c
	src/expansion_port.c
	)

target_sources_ifdef(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING app PRIVATE src/coap_prov_client.c)

target_sources_ifdef(CONFIG_SH_CMD app PRIVATE src/sh_cmd.c)

target_sources_ifdef(CONFIG_SH_CMD app PRIVATE src/modem_cmd.c)

target_sources_ifdef(CONFIG_SH_CMD_MODEM_TRACE app PRIVATE src/modem_trace_cmd.c)

target_sources_ifdef(CONFIG_UART_MANAGER app PRIVATE src/uart_manager.c)

target_sources_ifdef(CONFIG_UPDATE app PRIVATE src/appl_update.c)

target_sources_ifdef(CONFIG_UART_UPDATE app PRIVATE src/appl_update_xmodem.c)

target_sources_ifdef(CONFIG_COAP_UPDATE app PRIVATE src/appl_update_coap.c)

target_sources_ifdef(CONFIG_LOCATION_ENABLE app PRIVATE src/location.c)

target_sources_ifdef(CONFIG_MOTION_SENSOR app PRIVATE src/accelerometer_sensor.c)

target_sources_ifdef(CONFIG_ENVIRONMENT_SENSOR app PRIVATE src/environment_sensor.c src/environment_history.c)

target_sources_ifdef(CONFIG_SHT21 app PRIVATE src/environment_sht21.c src/environment_history.c)

target_sources_ifdef(CONFIG_BATTERY_ADC app PRIVATE src/battery_adc.c)

target_sources_ifdef(CONFIG_NAU7802_SCALE app PRIVATE src/nau7802.c)

target_sources_ifdef(CONFIG_USE_APPL_STORAGE app PRIVATE src/appl_storage.c src/appl_storage_config.c)

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

# UART cmds
zephyr_linker_sources(SECTIONS sh_cmds.ld)

# tinydtls - support DTLS 1.2 Connection ID
zephyr_library_link_libraries(tinydtls)

# APP END

if (COPY_PREBUILDS_FULL)
   # Enable west build -b ... -- -DCOPY_PREBUILDS_FULL=On
   # Copies the .hex files before the build!
   # Therefore build it first without,
   # and a second time with -DCOPY_PREBUILDS_FULL=On

   message(NOTICE "copy prebuild full binaries into ${CMAKE_BINARY_DIR}/../prebuild")
   if (EXISTS "${CMAKE_BINARY_DIR}/zephyr/merged.hex")
      file(COPY_FILE ${CMAKE_BINARY_DIR}/zephyr/merged.hex ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_full.hex)
   else()
      message(FATAL_ERROR "missing ${CMAKE_BINARY_DIR}/zephyr/merged.hex")
   endif()
endif()

if (COPY_PREBUILDS_UPDATE)
   # Enable west build -b ... -- -DCOPY_PREBUILDS_UPDATE=On
   # Copies the .hex files before the build!
   # Therefore build it first without,
   # and a second time with -DCOPY_PREBUILDS_UPDATE=On

   message(NOTICE "copy prebuild update binaries into ${CMAKE_BINARY_DIR}/../prebuild")
   if (EXISTS "${CMAKE_BINARY_DIR}/zephyr/app_update.bin")
      file(COPY_FILE ${CMAKE_BINARY_DIR}/zephyr/app_update.bin ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_app_update.bin)
   else()
      message(FATAL_ERROR "missing ${CMAKE_BINARY_DIR}/zephyr/app_update.bin")
   endif()
   if (EXISTS "${CMAKE_BINARY_DIR}/zephyr/app_signed.hex")
      file(COPY_FILE ${CMAKE_BINARY_DIR}/zephyr/app_signed.hex ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_app_update.hex)
   else()
      message(FATAL_ERROR "missing ${CMAKE_BINARY_DIR}/zephyr/app_signed.hex")
   endif()
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
elseif (CONFIG_DS18B20)
set(BUILD_SUMUP3 "Build with DS18B20.")
elseif (CONFIG_BME280)
set(BUILD_SUMUP3 "Build with BME280.")
elseif (CONFIG_DPS310)
set(BUILD_SUMUP3 "Build with DPS310.")
elseif (CONFIG_ENVIRONMENT_SENSOR)
set(BUILD_SUMUP3 "Build with env-sensor.")
else()
set(BUILD_SUMUP3 "Build without BME680/BME280/SHT21/SHT31/DS18B20/DPS310.")
endif()

if (CONFIG_ENVIRONMENT_SENSOR)
if (CONFIG_ENVIRONMENT_HISTORY_SIZE GREATER 0)
set(BUILD_SUMUP4 "Build with history using ${CONFIG_ENVIRONMENT_HISTORY_SIZE} values.")
else()
set(BUILD_SUMUP4 "Build without history.")
endif()
else()
set(BUILD_SUMUP4 "Build without history, no env-sensor.")
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
elseif (CONFIG_BATTERY_TYPE_NIMH_2000_MAH)
set(BUILD_SUMUP8 "Build for 2000 mAh NiMH")
elseif (CONFIG_BATTERY_TYPE_NIMH_4_2000_MAH)
set(BUILD_SUMUP8 "Build for 2000 mAh NiMH / 4 cells")
elseif (CONFIG_BATTERY_TYPE_SUPER_CAP_LIHY)
set(BUILD_SUMUP8 "Build for 400F LiHy")
else()
set(BUILD_SUMUP8 "Build without battery.")
endif()

if (CONFIG_LOG_BACKEND_UART_MANAGER)
set(BUILD_SUMUP9 "Build with UART manager log backend.")
elseif (CONFIG_UART_MANAGER)
set(BUILD_SUMUP9 "Build with UART manager.")
else()
set(BUILD_SUMUP9 "Build without UART manager.")
endif()

if (CONFIG_NRF_MODEM_LIB_TRACE)
if (CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART)
set(BUILD_SUMUP10 "Build with UART modem trace.")
elseif (CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_FLASH)
set(BUILD_SUMUP10 "Build with FLASH modem trace.")
elseif (CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_RAM)
set(BUILD_SUMUP10 "Build with RAM modem trace.")
elseif (CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_RTT)
set(BUILD_SUMUP10 "Build with RTT modem trace.")
else()
set(BUILD_SUMUP10 "Build unknown modem trace?")
endif()
else()
set(BUILD_SUMUP10 "Build without modem trace.")
endif()

if (CONFIG_INIT_SETTINGS)
set(BUILD_SUMUP11 "Build with initial settings.")
else()
set(BUILD_SUMUP11 "Build without initial settings.")
endif()

if (CONFIG_USE_APPL_STORAGE)
set(BUILD_SUMUP12 "Build with application storage.")
else()
set(BUILD_SUMUP12 "Build without application storage.")
endif()

message(STATUS "PM_STATIC_YML_FILE: ${PM_STATIC_YML_FILE}")

add_custom_command(TARGET app POST_BUILD
                   COMMAND echo "${BUILD_SUMUP1}"
                   COMMAND echo "${BUILD_SUMUP2}"
                   COMMAND echo "${BUILD_SUMUP3}"
                   COMMAND echo "${BUILD_SUMUP4}"
                   COMMAND echo "${BUILD_SUMUP5}"
                   COMMAND echo "${BUILD_SUMUP6}"
                   COMMAND echo "${BUILD_SUMUP7}"
                   COMMAND echo "${BUILD_SUMUP8}"
                   COMMAND echo "${BUILD_SUMUP9}"
                   COMMAND echo "${BUILD_SUMUP10}"
                   COMMAND echo "${BUILD_SUMUP11}"
                   COMMAND echo "${BUILD_SUMUP12}"
                   COMMAND echo "Build with network search timeout ${CONFIG_MODEM_SEARCH_TIMEOUT}s"
                   COMMAND echo "${CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION}"
                   )

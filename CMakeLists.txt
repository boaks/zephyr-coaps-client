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
target_sources(app PRIVATE src/main.c src/modem.c src/ui.c src/dtls_client.c src/dtls_credentials.c src/coap_client.c)

add_subdirectory_ifdef(CONFIG_EXTERNAL_SENSORS src/ext_sensors)

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
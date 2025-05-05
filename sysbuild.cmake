#
# Copyright (c) 2025 Achim Kraus CloudCoap.net
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

set(PM_STATIC_YML_FILE ${CMAKE_CURRENT_LIST_DIR}/configuration/pm_static.yml CACHE INTERNAL "")

message(STATUS "PM_STATIC_YML_FILE: ${PM_STATIC_YML_FILE}")

if (COPY_PREBUILDS_FULL)
   # Enable west build -b ... -- -DCOPY_PREBUILDS_FULL=On
   # Copies the .hex files before the build!
   # Therefore build it first without,
   # and a second time with -DCOPY_PREBUILDS_FULL=On

   message(NOTICE "copy prebuild full binaries into ${CMAKE_BINARY_DIR}/../prebuild")
   if (EXISTS "${CMAKE_BINARY_DIR}/merged.hex")
      file(COPY_FILE ${CMAKE_BINARY_DIR}/merged.hex ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_full.hex)
   else()
      message(FATAL_ERROR "missing ${CMAKE_BINARY_DIR}/merged.hex")
   endif()
endif()

if (COPY_PREBUILDS_UPDATE)
   # Enable west build -b ... -- -DCOPY_PREBUILDS_UPDATE=On
   # Copies the .hex files before the build!
   # Therefore build it first without,
   # and a second time with -DCOPY_PREBUILDS_UPDATE=On

   message(NOTICE "copy prebuild update binaries into ${CMAKE_BINARY_DIR}/../prebuild")
   if (EXISTS "${CMAKE_BINARY_DIR}/coaps-client/zephyr/cloudcoap.signed.bin")
      file(COPY_FILE ${CMAKE_BINARY_DIR}/coaps-client/zephyr/cloudcoap.signed.bin ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_app_update.bin)
   else()
      message(FATAL_ERROR "missing ${CMAKE_BINARY_DIR}/coaps-client/zephyr/cloudcoap.signed.bin")
   endif()
   if (EXISTS "${CMAKE_BINARY_DIR}/coaps-client/zephyr/cloudcoap.signed.hex")
      file(COPY_FILE ${CMAKE_BINARY_DIR}/coaps-client/zephyr/cloudcoap.signed.hex ${CMAKE_BINARY_DIR}/../prebuild/${BOARD}_app_update.hex)
   else()
      message(FATAL_ERROR "missing ${CMAKE_BINARY_DIR}/coaps-client/zephyr/cloudcoap.signed.hex")
   endif()
endif()


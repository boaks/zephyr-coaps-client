# Copyright (c) 2024 Circuit Dojo
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_CIRCUITDOJO_FEATHER_NRF9161_PMIC_STARTUP startup.c)

set(PM_STATIC_YML_FILE ${CMAKE_CURRENT_LIST_DIR}/circuitdojo_feather_nrf9161_pm_static.yml CACHE INTERNAL "")
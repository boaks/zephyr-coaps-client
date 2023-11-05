/*
 * Copyright (c) 2023 Achim Kraus CloudCoap.net
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0
 *
 * SPDX-License-Identifier: EPL-2.0
 */

#ifndef MODEM_DESC_H
#define MODEM_DESC_H

#include <modem/lte_lc.h>

#include "modem.h"

const char *modem_get_system_mode_description(
    enum lte_lc_system_mode lte_mode,
    enum lte_lc_system_mode_preference lte_preference);

const char *modem_get_system_mode_cfg(
    enum lte_lc_system_mode lte_mode,
    enum lte_lc_system_mode_preference lte_preference);

const char *modem_get_registration_description(enum lte_lc_nw_reg_status reg_status);

const char *modem_get_registration_short_description(enum lte_lc_nw_reg_status reg_status);

const char *modem_get_network_mode_description(enum lte_lc_lte_mode mode);

const char *modem_get_rai_description(enum lte_network_rai rai);

const char *modem_get_emm_cause_description(int cause);

#endif /* MODEM_DESC_H */

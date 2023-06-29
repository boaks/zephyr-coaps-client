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

#ifndef MODEM_SIM_H
#define MODEM_SIM_H

#include "modem.h"

void modem_sim_init(void);
bool modem_sim_multi_imsi(void);
bool modem_sim_apply_iccid_preference(void);
int modem_sim_get_info(struct lte_sim_info* info);
int modem_sim_read_info(struct lte_sim_info* info, bool init);

#endif /* MODEM_SIM_H */

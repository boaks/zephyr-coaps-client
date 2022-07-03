/*
 * Copyright (c) 2022 Achim Kraus CloudCoap.net
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

#include <logging/log.h>

#include "coap_client.h"
#include "dtls_client.h"

LOG_MODULE_REGISTER(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

void main(void)
{
   LOG_INF("CoAP/DTLS CID sample " CLIENT_VERSION " has started");
   dtls_loop();
}

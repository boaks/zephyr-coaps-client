/*
 * Copyright (c) 2023Achim Kraus CloudCoap.net
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/logging/log.h>

#include "transform.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

int32_t transform_curve(int32_t in_value,
                        const struct transform_curve *curve)
{
   int points = curve->points;
   const struct transform_point *pb = curve->curve;

   if (in_value >= pb->in_value) {
      /* in_value above curve */
      return pb->out_value;
   }
   --points;
   ++pb;
   /* search in_value */
   while ((points > 0) && (in_value < pb->in_value)) {
      --points;
      ++pb;
   }
   if (in_value < pb->in_value) {
      /* Below curve */
      return pb->out_value;
   }

   /* Linear interpolation between below and above points. */
   const struct transform_point *pa = pb - 1;

   return pb->out_value + ((pa->out_value - pb->out_value) * (in_value - pb->in_value) / (pa->in_value - pb->in_value));
}

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

#ifndef TRANSFORM_H_
#define TRANSFORM_H_

#include <stddef.h>

/** A transformation point.
 *
 * A transformation curve is defined as a sequence of these points.
 * The in_value must be monotonic decreasing within the sequence.
 */
struct transform_point {
   int32_t in_value;
   int32_t out_value;
};

struct transform_curve {
   size_t points;
   struct transform_point curve[];
};

/** Calculate the transformation based on the curve.
 *
 * @param in_value input value.
 *
 * @param curve the transformation curve.
 *
 * @return the interpolated out_value.
 */
int32_t transform_curve(int32_t in_value,
                                const struct transform_curve *curve);

#endif /* TRANSFORM_H_ */

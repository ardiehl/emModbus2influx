/*
 * Copyright © 2010-2014 Stéphane Raimbault <stephane.raimbault@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later

 * included here because different libmodbus version behave different regarding
 * modbus_get_float_xxxx
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _MSC_VER
#  include <stdint.h>
#else
#  include "stdint.h"
#endif

/* Get a float from 4 bytes (Modbus) without any conversion (ABCD) */
float mb_get_float_abcd(const uint16_t *src);

/* Get a float from 4 bytes (Modbus) in inversed format (DCBA) */
float mb_get_float_dcba(const uint16_t *src);

/* Get a float from 4 bytes (Modbus) with swapped bytes (BADC) */
float mb_get_float_badc(const uint16_t *src);

/* Get a float from 4 bytes (Modbus) with swapped words (CDAB) */
float mb_get_float_cdab(const uint16_t *src);

/* Set a float to 4 bytes for Modbus w/o any conversion (ABCD) */
void mb_set_float_abcd(float f, uint16_t *dest);

/* Set a float to 4 bytes for Modbus with byte and word swap conversion (DCBA) */
void mb_set_float_dcba(float f, uint16_t *dest);

/* Set a float to 4 bytes for Modbus with byte swap conversion (BADC) */
void mb_set_float_badc(float f, uint16_t *dest);

/* Set a float to 4 bytes for Modbus with word swap conversion (CDAB) */
void mb_set_float_cdab(float f, uint16_t *dest);

#ifdef __cplusplus
}
#endif

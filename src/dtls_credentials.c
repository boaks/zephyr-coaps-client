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

#include "dtls_credentials.h"

#include <stdio.h>

#include "dtls_debug.h"
#include "dtls_prng.h"

#ifdef __GNUC__
#define UNUSED_PARAM __attribute__((unused))
#else
#define UNUSED_PARAM
#endif /* __GNUC__ */

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#ifdef DTLS_ECC
static const unsigned char ecdsa_priv_key[] = {
    0x41, 0xC1, 0xCB, 0x6B, 0x51, 0x24, 0x7A, 0x14,
    0x43, 0x21, 0x43, 0x5B, 0x7A, 0x80, 0xE7, 0x14,
    0x89, 0x6A, 0x33, 0xBB, 0xAD, 0x72, 0x94, 0xCA,
    0x40, 0x14, 0x55, 0xA1, 0x94, 0xA9, 0x49, 0xFA};

static const unsigned char ecdsa_pub_key_x[] = {
    0x36, 0xDF, 0xE2, 0xC6, 0xF9, 0xF2, 0xED, 0x29,
    0xDA, 0x0A, 0x9A, 0x8F, 0x62, 0x68, 0x4E, 0x91,
    0x63, 0x75, 0xBA, 0x10, 0x30, 0x0C, 0x28, 0xC5,
    0xE4, 0x7C, 0xFB, 0xF2, 0x5F, 0xA5, 0x8F, 0x52};

static const unsigned char ecdsa_pub_key_y[] = {
    0x71, 0xA0, 0xD4, 0xFC, 0xDE, 0x1A, 0xB8, 0x78,
    0x5A, 0x3C, 0x78, 0x69, 0x35, 0xA7, 0xCF, 0xAB,
    0xE9, 0x3F, 0x98, 0x72, 0x09, 0xDA, 0xED, 0x0B,
    0x4F, 0xAB, 0xC3, 0x6F, 0xC7, 0x72, 0xF8, 0x29};
#endif /* DTLS_ECC */

#ifdef DTLS_PSK

/* The PSK information for DTLS */
static unsigned char psk_id[32] = { 0 };
static size_t psk_id_length = sizeof(psk_id);
static unsigned char psk_key[] = CONFIG_DTLS_PSK_SECRET;
static size_t psk_key_length = sizeof(psk_key) - 1;

/* This function is the "key store" for tinyDTLS. It is called to
 * retrieve a key for the given identity within this particular
 * session. */
static int
get_psk_info(dtls_context_t *ctx UNUSED_PARAM,
             const session_t *session UNUSED_PARAM,
             dtls_credentials_type_t type,
             const unsigned char *id, size_t id_len,
             unsigned char *result, size_t result_length)
{

   switch (type) {
      case DTLS_PSK_IDENTITY:
         if (id_len) {
            dtls_debug("got psk_identity_hint: '%.*s'\n", (int)id_len, id);
         }

         if (result_length < psk_id_length) {
            dtls_warn("cannot set psk_identity -- buffer too small\n");
            return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
         }

         memcpy(result, psk_id, psk_id_length);
         return psk_id_length;
      case DTLS_PSK_KEY:
         if (id_len != psk_id_length || memcmp(psk_id, id, id_len) != 0) {
            dtls_warn("PSK for unknown id requested, exiting\n");
            return dtls_alert_fatal_create(DTLS_ALERT_ILLEGAL_PARAMETER);
         } else if (result_length < psk_key_length) {
            dtls_warn("cannot set psk -- buffer too small\n");
            return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
         }

         memcpy(result, psk_key, psk_key_length);
         return psk_key_length;
      case DTLS_PSK_HINT:
      default:
         dtls_warn("unsupported request type: %d\n", type);
   }

   return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
}

void dtls_credentials_init_psk(const char *imei)
{
   unsigned char *cur;

   strncpy(psk_id, CONFIG_DTLS_PSK_IDENTITY, psk_id_length);
   cur = (unsigned char *)strstr(psk_id, "${imei}");
   if (cur) {
      if (imei && strlen(imei)) {
         snprintf(cur, sizeof(psk_id) - (cur - psk_id), "%s", imei);
      } else {
         uint32_t id = 0;
         dtls_prng((unsigned char *)&id, sizeof(id));
         snprintf(cur, sizeof(psk_id) - (cur - psk_id), "%u", id);
      }
   }
   dtls_info("psk-id: %s\n", psk_id);
   psk_id_length = strlen(psk_id);
}
#else /* DTLS_PSK */

void dtls_credentials_init_psk(const char *imei UNUSED_PARAM)
{
}

#endif /* DTLS_PSK */

#ifdef DTLS_ECC
static int
get_ecdsa_key(dtls_context_t *ctx UNUSED_PARAM,
              const session_t *session UNUSED_PARAM,
              const dtls_ecdsa_key_t **result)
{
   static const dtls_ecdsa_key_t ecdsa_key = {
       .curve = DTLS_ECDH_CURVE_SECP256R1,
       .priv_key = ecdsa_priv_key,
       .pub_key_x = ecdsa_pub_key_x,
       .pub_key_y = ecdsa_pub_key_y};

   *result = &ecdsa_key;
   return 0;
}

static int
verify_ecdsa_key(dtls_context_t *ctx UNUSED_PARAM,
                 const session_t *session UNUSED_PARAM,
                 const unsigned char *other_pub_x,
                 const unsigned char *other_pub_y,
                 size_t key_size)
{
   (void)other_pub_x;
   (void)other_pub_y;
   (void)key_size;
   return 0;
}
#endif /* DTLS_ECC */

void dtls_credentials_init_handler(dtls_handler_t *handler)
{
#ifdef DTLS_PSK
   handler->get_psk_info = get_psk_info;
#endif /* DTLS_PSK */
#ifdef DTLS_ECC
   handler->get_ecdsa_key = get_ecdsa_key;
   handler->verify_ecdsa_key = verify_ecdsa_key;
#endif /* DTLS_ECC */
}

const char *dtls_credentials_get_psk_identity(void)
{
#ifdef DTLS_PSK
   return psk_id;
   #else 
   return "cali.anonymous";
#endif
}

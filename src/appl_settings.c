/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/printk.h>

#include "crypto.h"
#include "dtls.h"
#include "dtls_prng.h"

#include "parse.h"
#include "sh_cmd.h"

#include "appl_settings.h"

#define STORAGE_PARTITION storage_partition
#define STORAGE_PARTITION_ID FIXED_PARTITION_ID(STORAGE_PARTITION)

#define MAX_SETTINGS_KEY_LENGTH (8 * 3)

#define SETTINGS_SERVICE_NAME "csrv"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#ifdef CONFIG_COAP_SERVER_PORT
#define DEFAUL_COAP_SERVER_PORT CONFIG_COAP_SERVER_PORT
#else
#define DEFAUL_COAP_SERVER_PORT 5683
#endif

#ifdef CONFIG_COAP_SERVER_SECURE_PORT
#define DEFAUL_COAP_SERVER_SECURE_PORT CONFIG_COAP_SERVER_SECURE_PORT
#else
#define DEFAUL_COAP_SERVER_SECURE_PORT 5684
#endif

#define SETTINGS_KEY_INIT "init"
#define SETTINGS_KEY_SCHEME "scheme"
#define SETTINGS_KEY_DESTINATION "dest"
#define SETTINGS_KEY_PORT "port"
#define SETTINGS_KEY_SECURE_PORT "sport"
#define SETTINGS_KEY_ID "id"
#define SETTINGS_KEY_COAP_PATH "path"
#define SETTINGS_KEY_COAP_QUERY "query"
#define SETTINGS_KEY_APN "apn"
#define SETTINGS_KEY_BATTERY_PROFILE "bat"

#define SETTINGS_KEY_PSK_ID "psk_id"
#define SETTINGS_KEY_PSK_KEY "psk_key"
#define SETTINGS_KEY_EC_PRIV "ec_priv"
#define SETTINGS_KEY_EC_PUB "ec_pub"
#define SETTINGS_KEY_EC_TRUST "ec_tr_pub"
#define SETTINGS_KEY_PROV "prov"
#define SETTINGS_KEY_UNLOCK "unlock"

static K_MUTEX_DEFINE(settings_mutex);

static uint8_t settings_initialized = 0;

#ifdef CONFIG_BATTERY_TYPE_DEFAULT
#define BATTERY_TYPE_DEFAULT CONFIG_BATTERY_TYPE_DEFAULT
#else
#define BATTERY_TYPE_DEFAULT 0
#endif

static uint8 battery_profile = BATTERY_TYPE_DEFAULT;

static char apn[MAX_SETTINGS_VALUE_LENGTH] = {0};
static char scheme[12] = "coaps";
static char destination[MAX_SETTINGS_VALUE_LENGTH] = "";
static uint16_t destination_port = DEFAUL_COAP_SERVER_PORT;
static uint16_t destination_secure_port = DEFAUL_COAP_SERVER_SECURE_PORT;
static char device_imei[DTLS_PSK_MAX_CLIENT_IDENTITY_LEN + 1] = {0};
static char device_id[DTLS_PSK_MAX_CLIENT_IDENTITY_LEN + 1] = {0};
static char coap_path[MAX_SETTINGS_VALUE_LENGTH] = {0};
static char coap_query[MAX_SETTINGS_VALUE_LENGTH] = {0};

#ifdef CONFIG_SH_CMD_UNLOCK
static unsigned char unlock_password[DTLS_PSK_MAX_KEY_LEN + 1] = {0};
#endif /* CONFIG_SH_CMD_UNLOCK */

#if defined(CONFIG_DTLS_PSK_SECRET) || defined(CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY) || defined(CONFIG_DTLS_ECDSA_PRIVATE_KEY) || defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY)

static int appl_settings_decode_value(const char *desc, const char *value, uint8_t *buf, size_t len)
{
   int res = 0;
   if (value && *value) {
      size_t value_len = strlen(value);
      if (*value == '\'') {
         if (1 < value_len && value[value_len - 1] == '\'') {
            res = MIN(value_len - 2, len - 1);
            memcpy(buf, &value[1], res);
            buf[res] = 0;
         } else {
            LOG_WRN("%s: ignore string value!", desc);
            res = -EINVAL;
         }
      } else if (*value == ':') {
         if (!strncmp(value, ":0x", 3) && (value_len % 2) == 1) {
            res = hex2bin(value + 3, value_len - 3, buf, len);
         } else {
            LOG_WRN("%s: ignore hex value!", desc);
            res = -EINVAL;
         }
      } else {
         size_t out_len = 0;
         res = base64_decode(buf, len, &out_len, value, value_len);
         if (!res) {
            res = out_len;
         } else {
            LOG_WRN("%s: ignore base64 value!", desc);
         }
      }
   }
   return res;
}

#endif /* CONFIG_DTLS_PSK_SECRET || CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY || CONFIG_DTLS_ECDSA_PRIVATE_KEY || CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY */

#ifdef DTLS_PSK

static unsigned char psk_id[DTLS_PSK_MAX_CLIENT_IDENTITY_LEN + 1] = {0};
static size_t psk_id_length = 0;

static unsigned char psk_key[DTLS_PSK_MAX_KEY_LEN] = {0};
static size_t psk_key_length = 0;

/* This function is the "key store" for tinyDTLS. It is called to
 * retrieve a key for the given identity within this particular
 * session. */
static int
appl_settings_get_psk_info(dtls_context_t *ctx,
                           const session_t *session,
                           dtls_credentials_type_t type,
                           const unsigned char *id, size_t id_len,
                           unsigned char *result, size_t result_length)
{
   ARG_UNUSED(ctx);
   ARG_UNUSED(session);
   int res = 0;

   switch (type) {
      case DTLS_PSK_IDENTITY:
         if (id_len) {
            LOG_DBG("got psk_identity_hint: '%.*s'", (int)id_len, id);
         }
         k_mutex_lock(&settings_mutex, K_FOREVER);
         if (result_length < psk_id_length) {
            LOG_WRN("cannot set psk_identity -- buffer too small");
            res = dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
         } else {
            memcpy(result, psk_id, psk_id_length);
            res = psk_id_length;
         }
         k_mutex_unlock(&settings_mutex);
         return res;
      case DTLS_PSK_KEY:
         k_mutex_lock(&settings_mutex, K_FOREVER);
         if (id_len != psk_id_length || memcmp(psk_id, id, id_len) != 0) {
            LOG_WRN("PSK for unknown id requested, exiting.");
            res = dtls_alert_fatal_create(DTLS_ALERT_ILLEGAL_PARAMETER);
         } else if (result_length < psk_key_length) {
            LOG_WRN("cannot set psk -- buffer too small.");
            res = dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
         } else {
            memcpy(result, psk_key, psk_key_length);
            res = psk_key_length;
         }
         k_mutex_unlock(&settings_mutex);
         return res;
      case DTLS_PSK_HINT:
      default:
         LOG_WRN("unsupported request type: %d.", type);
   }

   return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
}
#endif /* DTLS_PSK */

#ifdef DTLS_ECC

static const unsigned char ecdsa_pub_cert_asn1_header[] = {
    0x30, 0x59, /* SEQUENCE, length 89 bytes */
    0x30, 0x13, /* SEQUENCE, length 19 bytes */
    0x06, 0x07, /* OBJECT IDENTIFIER ecPublicKey (1 2 840 10045 2 1) */
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
    0x06, 0x08, /* OBJECT IDENTIFIER prime256v1 (1 2 840 10045 3 1 7) */
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07,
    0x03, 0x42, 0x00, /* BIT STRING, length 66 bytes, 0 bits unused */
    0x04              /* uncompressed, followed by the r und s values of the public key */
};

static bool is_zero(const unsigned char *key, size_t len)
{
   while (!*key && len > 0) {
      ++key;
      --len;
   }
   return len == 0;
}

#ifdef CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY

static int appl_settings_decode_public_key(const char *desc, const char *value, uint8_t *buf, size_t len)
{
   int res = 0;
   unsigned char temp[sizeof(ecdsa_pub_cert_asn1_header) + DTLS_EC_KEY_SIZE * 2];

   memset(temp, 0, sizeof(temp));
   memset(buf, 0, len);

   if (len >= DTLS_EC_KEY_SIZE * 2) {
      res = appl_settings_decode_value(desc, value, temp, sizeof(temp));

      if (res == sizeof(temp)) {
         if (memcmp(temp, ecdsa_pub_cert_asn1_header, sizeof(ecdsa_pub_cert_asn1_header))) {
            LOG_INF("%s: no SECP256R1 ASN.1 public key.", desc);
         } else {
            res = DTLS_EC_KEY_SIZE * 2;
            memmove(buf, &temp[sizeof(ecdsa_pub_cert_asn1_header)], res);
            sprintf(temp, "%s (from ASN.1):", desc);
            LOG_HEXDUMP_INF(buf, res, temp);
         }
      } else if (res == DTLS_EC_KEY_SIZE * 2) {
         memmove(buf, temp, res);
         sprintf(temp, "%s (from ASN.1):", desc);
         LOG_HEXDUMP_INF(buf, res, temp);
      } else {
         LOG_INF("%s: no SECP256R1 public key.", desc);
         res = 0;
      }
      if (is_zero(buf, res)) {
         res = 0;
         LOG_INF("no %s: disabled.", desc);
      }
   }
   return res;
}
#endif /* CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY */

#if defined(CONFIG_DTLS_ECDSA_PRIVATE_KEY) || defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY)

static const unsigned char ecdsa_priv_asn1_header[] = {
    0x30, 0x41,       /* SEQUENCE, length 65 bytes */
    0x02, 0x01, 0x00, /* INTEGER 0 */
    0x30, 0x13,       /* SEQUENCE, length 19 bytes */
    0x06, 0x07,       /* OBJECT IDENTIFIER ecPublicKey (1 2 840 10045 2 1) */
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
    0x06, 0x08, /* OBJECT IDENTIFIER prime256v1 (1 2 840 10045 3 1 7) */
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07,
    0x04, 0x27,       /* OCTET STRING, length 39 bytes */
    0x30, 0x25,       /* SEQUENCE, length 37 bytes */
    0x02, 0x01, 0x01, /* INTEGER 1 */
    0x04, 0x20,       /* OCTET STRING, length 32 bytes */
};

static const unsigned char ecdsa_priv_asn1_key[] = {
    0x30, 0x31,       /* SEQUENCE, length 49 bytes */
    0x02, 0x01, 0x01, /* INTEGER 1 */
    0x04, 0x20,       /* OCTET STRING, length 32 bytes */
};

static int appl_settings_decode_private_key(const char *desc, const char *value, uint8_t *buf, size_t len)
{
   int res = 0;
   unsigned char temp[sizeof(ecdsa_priv_asn1_header) + DTLS_EC_KEY_SIZE];

   memset(temp, 0, sizeof(temp));
   memset(buf, 0, len);

   if (len >= DTLS_EC_KEY_SIZE) {
      res = appl_settings_decode_value(desc, value, temp, sizeof(temp));

      if (res == sizeof(temp)) {
         if (memcmp(temp, ecdsa_priv_asn1_header, sizeof(ecdsa_priv_asn1_header)) == 0) {
            res = DTLS_EC_KEY_SIZE;
            memmove(buf, &temp[sizeof(ecdsa_priv_asn1_header)], res);
            sprintf(temp, "%s (from ASN.1):", desc);
            LOG_HEXDUMP_DBG(buf, res, temp);
         } else {
            LOG_INF("%s: no SECP256R1 ASN.1 private key.", desc);
         }
      } else if (res == (DTLS_EC_KEY_SIZE + sizeof(ecdsa_priv_asn1_key) + 12)) {
         if (memcmp(temp, ecdsa_priv_asn1_key, sizeof(ecdsa_priv_asn1_key)) == 0) {
            res = DTLS_EC_KEY_SIZE;
            memmove(buf, &temp[sizeof(ecdsa_priv_asn1_key)], res);
            sprintf(temp, "%s (from ASN.1):", desc);
            LOG_HEXDUMP_DBG(buf, res, temp);
         } else {
            LOG_INF("%s: no SECP256R1 ASN.1 private key.", desc);
         }
      } else if (res == DTLS_EC_KEY_SIZE) {
         memmove(buf, temp, res);
         sprintf(temp, "%s (from ASN.1):", desc);
         LOG_HEXDUMP_INF(buf, res, temp);
      } else {
         LOG_INF("%s: no SECP256R1 private key.", desc);
         res = 0;
      }
      if (is_zero(buf, res)) {
         res = 0;
         LOG_INF("no %s: disabled.", desc);
      }
   }
   return res;
}
#endif /* CONFIG_DTLS_ECDSA_PRIVATE_KEY || CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY */

static unsigned char ecdsa_priv_key[DTLS_EC_KEY_SIZE];

static unsigned char ecdsa_pub_key[DTLS_EC_KEY_SIZE * 2];

static unsigned char trusted_pub_key[DTLS_EC_KEY_SIZE * 2];

#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
static uint8_t ecdsa_provisioning_enabled = 0;
static unsigned char ecdsa_provisioning_priv_key[DTLS_EC_KEY_SIZE];
static unsigned char ecdsa_provisioning_pub_key[DTLS_EC_KEY_SIZE * 2];
#endif

static int
appl_settings_get_ecdsa_key(dtls_context_t *ctx,
                            const session_t *session,
                            const dtls_ecdsa_key_t **result)
{
   ARG_UNUSED(ctx);
   ARG_UNUSED(session);

   static const dtls_ecdsa_key_t ecdsa_key = {
       .curve = DTLS_ECDH_CURVE_SECP256R1,
       .priv_key = ecdsa_priv_key,
       .pub_key_x = ecdsa_pub_key,
       .pub_key_y = &ecdsa_pub_key[DTLS_EC_KEY_SIZE]};

#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
   static const dtls_ecdsa_key_t ecdsa_provisioning_key = {
       .curve = DTLS_ECDH_CURVE_SECP256R1,
       .priv_key = ecdsa_provisioning_priv_key,
       .pub_key_x = ecdsa_provisioning_pub_key,
       .pub_key_y = &ecdsa_provisioning_pub_key[DTLS_EC_KEY_SIZE]};

   LOG_INF("ecdsa %s", ecdsa_provisioning_enabled ? "provisioning" : "device");
   *result = ecdsa_provisioning_enabled ? &ecdsa_provisioning_key : &ecdsa_key;
#else
   *result = &ecdsa_key;
#endif
   return 0;
}

static int
appl_settings_verify_ecdsa_key(dtls_context_t *ctx,
                               const session_t *session,
                               const unsigned char *other_pub_x,
                               const unsigned char *other_pub_y,
                               size_t key_size)
{
   ARG_UNUSED(ctx);
   ARG_UNUSED(session);
   int res = 0;
   if (key_size != DTLS_EC_KEY_SIZE) {
      res = dtls_alert_fatal_create(DTLS_ALERT_UNSUPPORTED_CERTIFICATE);
   } else {
      k_mutex_lock(&settings_mutex, K_FOREVER);
      if (memcmp(trusted_pub_key, other_pub_x, key_size)) {
         res = dtls_alert_fatal_create(DTLS_ALERT_CERTIFICATE_UNKNOWN);
      } else if (memcmp(&trusted_pub_key[DTLS_EC_KEY_SIZE], other_pub_y, key_size)) {
         res = dtls_alert_fatal_create(DTLS_ALERT_CERTIFICATE_UNKNOWN);
      }
      k_mutex_unlock(&settings_mutex);
   }
   return res;
}
#endif /* DTLS_ECC */

static bool appl_settings_key_match(const char *name, const char *label, size_t len)
{
   return (len == strlen(label)) && !strncmp(name, label, len);
}

static int appl_settings_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                    void *cb_arg)
{
   int res = 0;
   const char *next;
   size_t name_len = settings_name_next(name, &next);

   LOG_INF("set: '%s'", name);

   if (!next) {
      bool sh_prot = sh_protected();
      uint16_t value = 0;
      char buf[MAX_SETTINGS_VALUE_LENGTH];

      if (appl_settings_key_match(name, SETTINGS_KEY_INIT, name_len)) {
         res = read_cb(cb_arg, &buf, sizeof(settings_initialized));
         k_mutex_lock(&settings_mutex, K_FOREVER);
         if (res == sizeof(settings_initialized)) {
            memcpy(&settings_initialized, buf, sizeof(settings_initialized));
         } else {
            settings_initialized = 0;
         }
         k_mutex_unlock(&settings_mutex);
         LOG_INF("init: %u", settings_initialized);
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_PROV, name_len)) {
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
         res = read_cb(cb_arg, &buf, sizeof(ecdsa_provisioning_enabled));
         k_mutex_lock(&settings_mutex, K_FOREVER);
         if (res == sizeof(ecdsa_provisioning_enabled)) {
            memcpy(&ecdsa_provisioning_enabled, buf, sizeof(ecdsa_provisioning_enabled));
         } else {
            ecdsa_provisioning_enabled = 0;
         }
         k_mutex_unlock(&settings_mutex);
         LOG_INF("provisioning: %u", ecdsa_provisioning_enabled);
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_BATTERY_PROFILE, name_len)) {
         res = read_cb(cb_arg, &buf, sizeof(battery_profile));
         k_mutex_lock(&settings_mutex, K_FOREVER);
         if (res == sizeof(battery_profile)) {
            memcpy(&battery_profile, buf, sizeof(battery_profile));
         } else {
            battery_profile = BATTERY_TYPE_DEFAULT;
         }
         k_mutex_unlock(&settings_mutex);
         LOG_INF("bat: %u", battery_profile);
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_PORT, name_len)) {
         res = read_cb(cb_arg, &value, sizeof(destination_port));
         k_mutex_lock(&settings_mutex, K_FOREVER);
         destination_port = value;
         k_mutex_unlock(&settings_mutex);
         LOG_INF("port: %u", value);
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_SECURE_PORT, name_len)) {
         res = read_cb(cb_arg, &value, sizeof(destination_secure_port));
         k_mutex_lock(&settings_mutex, K_FOREVER);
         destination_secure_port = value;
         k_mutex_unlock(&settings_mutex);
         LOG_INF("secure port: %u", value);
         return 0;
      }

      memset(buf, 0, sizeof(buf));

      if (appl_settings_key_match(name, SETTINGS_KEY_APN, name_len)) {
         res = read_cb(cb_arg, &buf, sizeof(apn) - 1);
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(apn, buf, sizeof(apn));
         k_mutex_unlock(&settings_mutex);
         if (res > 0) {
            LOG_INF("apn: '%s'", buf);
         }
         return 0;
      }
      if (appl_settings_key_match(name, SETTINGS_KEY_SCHEME, name_len)) {
         res = read_cb(cb_arg, &buf, sizeof(scheme) - 1);
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(scheme, buf, sizeof(scheme));
         k_mutex_unlock(&settings_mutex);
         if (res > 0) {
            LOG_INF("scheme: '%s'", buf);
         }
         return 0;
      }
      if (appl_settings_key_match(name, SETTINGS_KEY_DESTINATION, name_len)) {
         res = read_cb(cb_arg, &buf, sizeof(destination) - 1);
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(destination, buf, sizeof(destination));
         k_mutex_unlock(&settings_mutex);
         if (res > 0) {
            LOG_INF("dest: '%s'", buf);
         }
         return 0;
      }
      if (appl_settings_key_match(name, SETTINGS_KEY_COAP_PATH, name_len)) {
         res = read_cb(cb_arg, &buf, sizeof(coap_path) - 1);
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(coap_path, buf, sizeof(coap_path));
         k_mutex_unlock(&settings_mutex);
         if (res > 0) {
            LOG_INF("coap-path: '%s'", buf);
         }
         return 0;
      }
      if (appl_settings_key_match(name, SETTINGS_KEY_COAP_QUERY, name_len)) {
         res = read_cb(cb_arg, &buf, sizeof(coap_query) - 1);
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(coap_query, buf, sizeof(coap_query));
         k_mutex_unlock(&settings_mutex);
         if (res > 0) {
            LOG_INF("coap-query: '%s'", buf);
         }
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_ID, name_len)) {
         res = read_cb(cb_arg, &buf, sizeof(device_id) - 1);
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(device_id, buf, sizeof(device_id));
         k_mutex_unlock(&settings_mutex);
         if (res > 0) {
            LOG_INF("device_id: '%s'", buf);
         }
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_UNLOCK, name_len)) {
#ifdef CONFIG_SH_CMD_UNLOCK
         res = read_cb(cb_arg, &buf, sizeof(unlock_password) - 1);
         k_mutex_lock(&settings_mutex, K_FOREVER);
         if (res > 0) {
            memcpy(unlock_password, buf, sizeof(unlock_password));
         }
         k_mutex_unlock(&settings_mutex);
         if (res > 0) {
            LOG_INF("unlock: %d bytes", res);
         }
#endif /* CONFIG_SH_CMD_UNLOCK */
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_PSK_ID, name_len)) {
         res = read_cb(cb_arg, &buf, sizeof(psk_id) - 1);
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(psk_id, buf, sizeof(psk_id));
         psk_id_length = res > 0 ? res : 0;
         k_mutex_unlock(&settings_mutex);
         if (res > 0) {
            LOG_INF("psk_id: '%s'", psk_id);
         }
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_PSK_KEY, name_len)) {
#ifdef DTLS_PSK
         res = read_cb(cb_arg, &buf, sizeof(psk_key) - 1);
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(psk_key, buf, sizeof(psk_key));
         psk_key_length = res > 0 ? res : 0;
         k_mutex_unlock(&settings_mutex);
         if (res > 0) {
            LOG_INF("psk_key: %d bytes", res);
            if (!sh_prot) {
               LOG_HEXDUMP_INF(buf, res, name);
            }
         }
         k_mutex_unlock(&settings_mutex);
#endif /* DTLS_PSK */
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_EC_PRIV, name_len)) {
#ifdef DTLS_ECC
         res = read_cb(cb_arg, &buf, sizeof(ecdsa_priv_key));
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(ecdsa_priv_key, buf, sizeof(ecdsa_priv_key));
         memset(ecdsa_pub_key, 0, sizeof(ecdsa_pub_key));
         if (res > 0) {
            dtls_ecdsa_generate_public_key2(ecdsa_priv_key, ecdsa_pub_key, DTLS_EC_KEY_SIZE, TLS_EXT_ELLIPTIC_CURVES_SECP256R1);
            if (is_zero(ecdsa_priv_key, sizeof(ecdsa_priv_key))) {
               res = 0;
            }
         }
         k_mutex_unlock(&settings_mutex);
         if (!res) {
            LOG_INF("ecdsa_priv_key: zero");
         } else if (res > 0) {
            LOG_INF("ecdsa_priv_key: %d bytes", res);
         }
#endif /* DTLS_ECC */
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_EC_TRUST, name_len)) {
#ifdef DTLS_ECC
         res = read_cb(cb_arg, &buf, sizeof(trusted_pub_key));
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memcpy(trusted_pub_key, buf, sizeof(trusted_pub_key));
         if (is_zero(trusted_pub_key, sizeof(trusted_pub_key))) {
            res = 0;
         }
         k_mutex_unlock(&settings_mutex);
         if (!res) {
            LOG_INF("trusted_pub_key: zero");
         } else if (res > 0) {
            LOG_INF("trusted_pub_key: %d bytes", res);
         }
#endif /* DTLS_ECC */
         return 0;
      }
   }
   LOG_INF("set: '%s' unknown", name);

   return -ENOENT;
}

static int appl_settings_handle_export(int (*cb)(const char *name,
                                                 const void *value, size_t val_len))
{
   LOG_INF("export <" SETTINGS_SERVICE_NAME ">\n");
   k_mutex_lock(&settings_mutex, K_FOREVER);
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_INIT, &settings_initialized, sizeof(settings_initialized));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_PORT, &destination_port, sizeof(destination_port));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_SECURE_PORT, &destination_secure_port, sizeof(destination_secure_port));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_ID, device_id, strlen(device_id));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_SCHEME, scheme, strlen(scheme));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_DESTINATION, destination, strlen(destination));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_COAP_PATH, coap_path, strlen(coap_path));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_COAP_QUERY, coap_query, strlen(coap_query));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_APN, apn, strlen(apn));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_BATTERY_PROFILE, &battery_profile, sizeof(battery_profile));
#ifdef CONFIG_SH_CMD_UNLOCK
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_UNLOCK, unlock_password, strlen(unlock_password));
#endif

#ifdef DTLS_PSK
   if (psk_id_length && psk_key_length) {
      (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_PSK_ID, psk_id, psk_id_length);
      (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_PSK_KEY, psk_key, psk_key_length);
   } else {
      psk_id_length = 0;
      psk_key_length = 0;
      memset(psk_id, 0, sizeof(psk_id));
      memset(psk_key, 0, sizeof(psk_key));
   }
#endif /* DTLS_PSK */
#ifdef DTLS_ECC
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_EC_PRIV, ecdsa_priv_key, sizeof(ecdsa_priv_key));
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_EC_TRUST, trusted_pub_key, sizeof(trusted_pub_key));
#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
   (void)cb(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_PROV, &ecdsa_provisioning_enabled, sizeof(ecdsa_provisioning_enabled));
#endif
#endif /* DTLS_ECC */
   k_mutex_unlock(&settings_mutex);
   return 0;
}

static int appl_settings_handle_commit(void)
{
   LOG_INF("loading <" SETTINGS_SERVICE_NAME "> is done\n");
   return 0;
}

static int appl_settings_copy(const char *value, char *buf, size_t len)
{
   int res = 0;

   if (buf) {
      memset(buf, 0, len);
   }
   k_mutex_lock(&settings_mutex, K_FOREVER);
   res = strlen(value) + 1;
   if (res > len) {
      res = -EINVAL;
   } else {
      if (buf) {
         memmove(buf, value, res);
      }
      --res;
   }
   k_mutex_unlock(&settings_mutex);

   return res;
}

static int appl_settings_handle_get(const char *name, char *val, int val_len_max)
{
   int res = 0;
   const char *next;
   size_t name_len = settings_name_next(name, &next);

   LOG_INF("get: '%s'", name);
   name_len = settings_name_next(name, &next);
   memset(val, 0, val_len_max);

   if (!next) {
      bool sh_prot = sh_protected();

      if (appl_settings_key_match(name, SETTINGS_KEY_INIT, name_len)) {
         res = sizeof(settings_initialized);
         if (res > val_len_max) {
            return -EINVAL;
         }
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memmove(val, &settings_initialized, res);
         LOG_DBG("init: %u", settings_initialized);
         k_mutex_unlock(&settings_mutex);
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_PROV, name_len)) {
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
         res = sizeof(ecdsa_provisioning_enabled);
         if (res > val_len_max) {
            return -EINVAL;
         }
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memmove(val, &ecdsa_provisioning_enabled, res);
         LOG_DBG("provisioning: %u", ecdsa_provisioning_enabled);
         k_mutex_unlock(&settings_mutex);
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_BATTERY_PROFILE, name_len)) {
         res = sizeof(battery_profile);
         if (res > val_len_max) {
            return -EINVAL;
         }
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memmove(val, &battery_profile, res);
         LOG_INF("bat: %u", battery_profile);
         k_mutex_unlock(&settings_mutex);
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_PORT, name_len)) {
         res = sizeof(destination_port);
         if (res > val_len_max) {
            return -EINVAL;
         }
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memmove(val, &destination_port, res);
         LOG_INF("port: %u", destination_port);
         k_mutex_unlock(&settings_mutex);
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_SECURE_PORT, name_len)) {
         res = sizeof(destination_secure_port);
         if (res > val_len_max) {
            return -EINVAL;
         }
         k_mutex_lock(&settings_mutex, K_FOREVER);
         memmove(val, &destination_secure_port, res);
         LOG_INF("secure port: %u", destination_secure_port);
         k_mutex_unlock(&settings_mutex);
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_APN, name_len)) {
         res = appl_settings_copy(apn, val, val_len_max);
         if (res >= 0) {
            LOG_DBG("apn: '%s'", val);
         }
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_SCHEME, name_len)) {
         res = appl_settings_copy(scheme, val, val_len_max);
         if (res >= 0) {
            LOG_DBG("scheme: '%s'", val);
         }
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_DESTINATION, name_len)) {
         res = appl_settings_copy(destination, val, val_len_max);
         if (res >= 0) {
            LOG_DBG("dest: '%s'", val);
         }
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_COAP_PATH, name_len)) {
         res = appl_settings_copy(coap_path, val, val_len_max);
         if (res >= 0) {
            LOG_DBG("coap-path: '%s'", val);
         }
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_COAP_QUERY, name_len)) {
         res = appl_settings_copy(coap_query, val, val_len_max);
         if (res >= 0) {
            LOG_DBG("coap-query: '%s'", val);
         }
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_ID, name_len)) {
         res = appl_settings_copy(device_id, val, val_len_max);
         if (res >= 0) {
            LOG_DBG("device-id: '%s'", val);
         }
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_PSK_ID, name_len)) {
#ifdef DTLS_PSK
         res = appl_settings_copy(psk_id, val, val_len_max);
         if (res >= 0) {
            LOG_DBG("psk-id: '%s'", val);
         }
#endif /* DTLS_PSK */
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_PSK_KEY, name_len)) {
#ifdef DTLS_PSK
         k_mutex_lock(&settings_mutex, K_FOREVER);
         res = psk_key_length;
         k_mutex_unlock(&settings_mutex);
         if (res) {
            if (sh_prot) {
               LOG_INF("Get: '%s' protected!", name);
               res = 0;
            } else {
               res = appl_settings_copy(psk_key, val, val_len_max);
               if (res >= 0) {
                  LOG_DBG("Get: '%s' %d bytes", name, res);
                  LOG_HEXDUMP_DBG(val, res, name);
               }
            }
         } else {
            LOG_DBG("Get: '%s' 0 bytes", name);
         }
#endif /* DTLS_PSK */
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_EC_PRIV, name_len)) {
#ifdef DTLS_ECC
         k_mutex_lock(&settings_mutex, K_FOREVER);
         res = is_zero(ecdsa_priv_key, sizeof(ecdsa_priv_key));
         k_mutex_unlock(&settings_mutex);
         if (!res) {
            LOG_INF("Get: '%s' protected!", name);
         } else {
            LOG_DBG("Get: '%s' 0 bytes", name);
         }
#endif /* DTLS_ECC */
         return 0;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_EC_PUB, name_len)) {
#ifdef DTLS_ECC
         k_mutex_lock(&settings_mutex, K_FOREVER);
         if (!is_zero(ecdsa_pub_key, sizeof(ecdsa_pub_key))) {
            res = sizeof(ecdsa_pub_key);
            if (res > val_len_max) {
               res = -EINVAL;
            } else {
               memmove(val, ecdsa_pub_key, res);
            }
         }
         k_mutex_unlock(&settings_mutex);
         if (res >= 0) {
            LOG_DBG("Get: '%s' %d bytes", name, res);
            LOG_HEXDUMP_DBG(val, res, name);
         }
#endif /* DTLS_ECC */
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_EC_TRUST, name_len)) {
#ifdef DTLS_ECC
         k_mutex_lock(&settings_mutex, K_FOREVER);
         if (!is_zero(trusted_pub_key, sizeof(trusted_pub_key))) {
            res = sizeof(trusted_pub_key);
            if (res > val_len_max) {
               res = -EINVAL;
            } else {
               memmove(val, trusted_pub_key, res);
            }
         }
         k_mutex_unlock(&settings_mutex);
         if (res >= 0) {
            LOG_DBG("Get: '%s' %d bytes", name, res);
            LOG_HEXDUMP_DBG(val, res, name);
         }
#endif /* DTLS_ECC */
         return res;
      }

      if (appl_settings_key_match(name, SETTINGS_KEY_UNLOCK, name_len)) {
#ifdef CONFIG_SH_CMD_UNLOCK
         k_mutex_lock(&settings_mutex, K_FOREVER);
         res = unlock_password[0];
         k_mutex_unlock(&settings_mutex);
         if (res) {
            LOG_INF("Get: '%s' protected!", name);
         } else {
            LOG_DBG("Get: '%s' 0 bytes", name);
         }
#endif /* CONFIG_SH_CMD_UNLOCK */
         return 0;
      }
   }
   LOG_WRN("get: '%s' unknown", name);

   return -ENOENT;
}

/* static subtree handler */
SETTINGS_STATIC_HANDLER_DEFINE(cloud_service, SETTINGS_SERVICE_NAME, appl_settings_handle_get,
                               appl_settings_handle_set, appl_settings_handle_commit,
                               appl_settings_handle_export);

static int appl_settings_initialize(void)
{
   int res = settings_subsys_init();
   if (res) {
      LOG_WRN("Settings subsys initialization: fail (err %d, %s)", res, strerror(-res));
   } else {
      LOG_INF("Settings subsys initialized.");
      settings_load();
   }

   return res;
}

SYS_INIT(appl_settings_initialize, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#define SETTINGS_RESET_DEST 1
#define SETTINGS_RESET_ID 2
#define SETTINGS_RESET_UNLOCK 4
#define SETTINGS_RESET_PSK 8
#define SETTINGS_RESET_ECDSA 16
#define SETTINGS_RESET_TRUST 32
#define SETTINGS_RESET_PROVISIONING 64

#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
static int appl_settings_init_provisioning(void)
{
   size_t len;
   memset(ecdsa_provisioning_priv_key, 0, sizeof(ecdsa_provisioning_priv_key));
   memset(ecdsa_provisioning_pub_key, 0, sizeof(ecdsa_provisioning_pub_key));
   len = appl_settings_decode_private_key("ecdsa provisioning private key", CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY, ecdsa_provisioning_priv_key, sizeof(ecdsa_provisioning_priv_key));
   if (len == DTLS_EC_KEY_SIZE) {
      return dtls_ecdsa_generate_public_key2(ecdsa_provisioning_priv_key, ecdsa_provisioning_pub_key, DTLS_EC_KEY_SIZE, TLS_EXT_ELLIPTIC_CURVES_SECP256R1);
   } else if (len > 0) {
      LOG_ERR("ecdsa provisioning private key: %d != %d wrong length.", len, DTLS_EC_KEY_SIZE);
   }
   LOG_ERR("ecdsa provisioning disabled.");
   memset(ecdsa_provisioning_priv_key, 0, sizeof(ecdsa_provisioning_priv_key));
   return 0;
}
#endif

#ifdef CONFIG_COAP_SERVER_HOSTNAME
#define CONFIG_COAP_SERVER CONFIG_COAP_SERVER_HOSTNAME
#elif defined(CONFIG_COAP_SERVER_ADDRESS_STATIC)
#define CONFIG_COAP_SERVER CONFIG_COAP_SERVER_ADDRESS_STATIC
#else
#define CONFIG_COAP_SERVER "californium.eclipseprojects.io"
#endif

#if defined(CONFIG_DEVICE_IDENTITY) || defined(CONFIG_DTLS_PSK_IDENTITY) || defined(CONFIG_COAP_RESOURCE) || defined(CONFIG_COAP_QUERY)
static int appl_settings_expand_imei(char *buf, size_t size, const char *value)
{
   char *cur;
   size_t id_len;

   strncpy(buf, value, size - 1);
   id_len = strlen(buf);
   cur = strstr(buf, "${imei}");
   if (cur) {
      size_t imei_len = strlen(device_imei);
      size_t free_len = size - id_len + 6;
      if (imei_len > free_len) {
         imei_len = free_len;
      }
      memmove(cur + imei_len, cur + 7, id_len - 6 - (cur - buf));
      memmove(cur, device_imei, imei_len);
      id_len += imei_len - 7;
   }
   return id_len;
}
#endif /* CONFIG_DEVICE_IDENTITY || CONFIG_DTLS_PSK_IDENTITY || CONFIG_COAP_RESOURCE || CONFIG_COAP_QUERY */

static void appl_setting_factory_reset(int flags)
{
   bool save = false;
   int res = 0;

   k_mutex_lock(&settings_mutex, K_FOREVER);
   if (flags & SETTINGS_RESET_DEST) {
      memset(scheme, 0, sizeof(scheme));
      memset(destination, 0, sizeof(destination));
      memset(coap_path, 0, sizeof(coap_path));
      memset(coap_query, 0, sizeof(coap_query));

      destination_port = DEFAUL_COAP_SERVER_PORT;
      destination_secure_port = DEFAUL_COAP_SERVER_SECURE_PORT;
#ifdef CONFIG_COAP_SERVER_HOSTNAME
      strncpy(destination, CONFIG_COAP_SERVER_HOSTNAME, sizeof(destination) - 1);
#elif defined(CONFIG_COAP_SERVER_ADDRESS_STATIC)
      strncpy(destination, CONFIG_COAP_SERVER_ADDRESS_STATIC, sizeof(destination) - 1);
#endif
#ifdef CONFIG_COAP_SERVER_PORT
      destination_port = CONFIG_COAP_SERVER_PORT;
#endif /* CONFIG_COAP_SERVER_PORT */
#ifdef CONFIG_COAP_SERVER_SECURE_PORT
      destination_secure_port = CONFIG_COAP_SERVER_SECURE_PORT;
#endif /* CONFIG_COAP_SERVER_SECURE_PORT */
#ifdef CONFIG_COAP_SCHEME
      strncpy(scheme, CONFIG_COAP_SCHEME, sizeof(scheme) - 1);
#else
      strncpy(scheme, "coaps", sizeof(scheme) - 1);
#endif
      LOG_INF("dest: %s://%s:%u/%u", scheme, destination, destination_port, destination_secure_port);

#ifdef CONFIG_COAP_RESOURCE
      appl_settings_expand_imei(coap_path, sizeof(coap_path), CONFIG_COAP_RESOURCE);
#endif
#ifdef CONFIG_COAP_QUERY
      appl_settings_expand_imei(coap_query, sizeof(coap_query), CONFIG_COAP_QUERY);
#endif
      save = true;
   }

   if (flags & SETTINGS_RESET_ID) {
      memset(device_id, 0, sizeof(device_id));
#ifdef CONFIG_DEVICE_IDENTITY
      appl_settings_expand_imei(device_id, sizeof(device_id), CONFIG_DEVICE_IDENTITY);
#endif /* CONFIG_DEVICE_IDENTITY */
      LOG_INF("device-id: %s", device_id);
      battery_profile = BATTERY_TYPE_DEFAULT;
      save = true;
   }

#ifdef DTLS_PSK
   if (flags & SETTINGS_RESET_PSK) {
      psk_id_length = 0;
      psk_key_length = 0;
      memset(psk_id, 0, sizeof(psk_id));
      memset(psk_key, 0, sizeof(psk_key));

#ifdef CONFIG_DTLS_PSK_IDENTITY
      psk_id_length = appl_settings_expand_imei(psk_id, sizeof(psk_id), CONFIG_DTLS_PSK_IDENTITY);
#endif /* CONFIG_DTLS_PSK_IDENTITY */
      LOG_INF("psk-id: %s", psk_id);
      if (psk_id_length) {
#ifdef CONFIG_DTLS_PSK_SECRET_GENERATE
         dtls_prng(psk_key, 12);
         res = 12;
#elif defined(CONFIG_DTLS_PSK_SECRET)
         res = appl_settings_decode_value("psk-secret", CONFIG_DTLS_PSK_SECRET, psk_key, sizeof(psk_key));
#else
         res = 0;
#endif
         if (res > 0) {
            psk_key_length = res;
            LOG_INF("psk-secret: %d", res);
            LOG_HEXDUMP_INF(psk_key, psk_key_length, "psk:");
         } else {
            psk_id_length = 0;
            psk_key_length = 0;
            memset(psk_id, 0, sizeof(psk_id));
            LOG_INF("no psk-secret, disabled");
         }
      }
      save = true;
   }
#endif /* DTLS_PSK */

#ifdef DTLS_ECC
   if (flags & SETTINGS_RESET_ECDSA) {
      memset(ecdsa_priv_key, 0, sizeof(ecdsa_priv_key));
      memset(ecdsa_pub_key, 0, sizeof(ecdsa_pub_key));

#ifdef CONFIG_DTLS_ECDSA_PRIVATE_KEY_GENERATE
      if (dtls_ecdsa_generate_key2(ecdsa_priv_key, ecdsa_pub_key, DTLS_EC_KEY_SIZE, TLS_EXT_ELLIPTIC_CURVES_SECP256R1)) {
         LOG_INF("ecdsa private key: generated.");
         LOG_HEXDUMP_INF(ecdsa_pub_key, sizeof(ecdsa_pub_key), "generated device public key:");
      } else {
         LOG_INF("ecdsa private key: failed to generate, disabled.");
      }
#elif defined(CONFIG_DTLS_ECDSA_PRIVATE_KEY)
      res = appl_settings_decode_private_key("ecdsa private key", CONFIG_DTLS_ECDSA_PRIVATE_KEY, ecdsa_priv_key, sizeof(ecdsa_priv_key));
      if (res > 0) {
         if (res == DTLS_EC_KEY_SIZE) {
            dtls_ecdsa_generate_public_key2(ecdsa_priv_key, ecdsa_pub_key, DTLS_EC_KEY_SIZE, TLS_EXT_ELLIPTIC_CURVES_SECP256R1);
            LOG_HEXDUMP_INF(ecdsa_pub_key, sizeof(ecdsa_pub_key), "device public key:");
         } else {
            memset(ecdsa_priv_key, 0, sizeof(ecdsa_priv_key));
            LOG_ERR("ecdsa private key: %d != %d wrong length.", res, DTLS_EC_KEY_SIZE);
         }
      }
#endif
      if (is_zero(ecdsa_priv_key, sizeof(ecdsa_priv_key))) {
         LOG_INF("ecdsa no private key: disabled.");
      }
      save = true;
   }

#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
   if (flags & SETTINGS_RESET_PROVISIONING) {
      if (appl_settings_init_provisioning()) {
         ecdsa_provisioning_enabled = 1;
         LOG_INF("ecdsa provisioning enabled.");
         save = true;
      }
   }
#endif /* CONFIG_DTLS_ECDSA_PROVISIONING */

   if (flags & SETTINGS_RESET_TRUST) {
      memset(trusted_pub_key, 0, sizeof(trusted_pub_key));
#ifdef CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY
      res = appl_settings_decode_public_key("trusted public key", CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY, trusted_pub_key, sizeof(trusted_pub_key));
#else
      res = 0;
#endif /* CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY */
      if (is_zero(trusted_pub_key, sizeof(trusted_pub_key))) {
         LOG_INF("ecdsa no trusted public key: disabled.");
      }
      save = true;
   }
#endif /* DTLS_ECC */

#ifdef CONFIG_SH_CMD_UNLOCK
   if (flags & SETTINGS_RESET_UNLOCK) {
      memset(unlock_password, 0, sizeof(unlock_password));
#ifdef CONFIG_SH_CMD_UNLOCK_PASSWORD
      strncpy(unlock_password, CONFIG_SH_CMD_UNLOCK_PASSWORD, sizeof(unlock_password) - 1);
#endif
      save = true;
   }
#endif /* CONFIG_SH_CMD_UNLOCK */

   if (save || !settings_initialized) {
      settings_initialized = 1;
      settings_save();
   }
   k_mutex_unlock(&settings_mutex);
}

void appl_settings_init(const char *imei, dtls_handler_t *handler)
{
   if (imei) {
      k_mutex_lock(&settings_mutex, K_FOREVER);
      memset(device_imei, 0, sizeof(device_imei));
      strncpy(device_imei, imei, sizeof(device_imei) - 1);
      k_mutex_unlock(&settings_mutex);
   }

   if (settings_initialized) {
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
      if (handler && ecdsa_provisioning_enabled) {
         if (!appl_settings_init_provisioning()) {
            appl_settings_provisioning_done();
         }
      }
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
   } else {
      if (handler) {
         appl_setting_factory_reset(SETTINGS_RESET_DEST | SETTINGS_RESET_ID | SETTINGS_RESET_UNLOCK |
                                    SETTINGS_RESET_PSK | SETTINGS_RESET_ECDSA | SETTINGS_RESET_TRUST |
                                    SETTINGS_RESET_PROVISIONING);
      } else {
         appl_setting_factory_reset(SETTINGS_RESET_DEST | SETTINGS_RESET_ID | SETTINGS_RESET_UNLOCK);
      }
   }

   if (handler) {

#ifdef DTLS_PSK
      k_mutex_lock(&settings_mutex, K_FOREVER);
      if (psk_id_length && psk_key_length) {
         handler->get_psk_info = appl_settings_get_psk_info;
         LOG_INF("Enable PSK");
      }
      k_mutex_unlock(&settings_mutex);
#endif /* DTLS_PSK */
#ifdef DTLS_ECC
      k_mutex_lock(&settings_mutex, K_FOREVER);
      if (!is_zero(ecdsa_priv_key, sizeof(ecdsa_priv_key)) &&
          !is_zero(ecdsa_pub_key, sizeof(ecdsa_pub_key))) {
         handler->get_ecdsa_key = appl_settings_get_ecdsa_key;
         LOG_INF("Enable ECDSA");
      }
      if (!is_zero(trusted_pub_key, sizeof(trusted_pub_key))) {
         handler->verify_ecdsa_key = appl_settings_verify_ecdsa_key;
         LOG_INF("Enable ECDSA trust");
      }
      k_mutex_unlock(&settings_mutex);
#endif /* DTLS_ECC */
   }
}

int appl_settings_get_apn(char *buf, size_t len)
{
   return appl_settings_copy(apn, buf, len);
}

int appl_settings_get_device_identity(char *buf, size_t len)
{
   return appl_settings_copy(device_id, buf, len);
}

int appl_settings_get_scheme(char *buf, size_t len)
{
   return appl_settings_copy(scheme, buf, len);
}

int appl_settings_get_destination(char *buf, size_t len)
{
   return appl_settings_copy(destination, buf, len);
}

int appl_settings_get_coap_path(char *buf, size_t len)
{
   return appl_settings_copy(coap_path, buf, len);
}

int appl_settings_get_coap_query(char *buf, size_t len)
{
   return appl_settings_copy(coap_query, buf, len);
}

uint16_t appl_settings_get_destination_port(bool secure)
{
   uint16_t port = 0;

   k_mutex_lock(&settings_mutex, K_FOREVER);
   port = secure ? destination_secure_port : destination_port;
   k_mutex_unlock(&settings_mutex);

   return port;
}

int appl_settings_get_battery_profile(void)
{
   return battery_profile;
}

#if defined(DTLS_ECC)

static uint8 *
appl_settings_add_ecdsa_signature_elem(uint8 *p, uint32_t *point_r, uint32_t *point_s)
{
   int len_r;
   int len_s;

#define R_KEY_OFFSET (1 + 1 + 2 + 1 + 1)
#define S_KEY_OFFSET(len_a) (R_KEY_OFFSET + (len_a))
   /* store the pointer to the r component of the signature and make space */
   len_r = dtls_ec_key_asn1_from_uint32(point_r, DTLS_EC_KEY_SIZE, p + R_KEY_OFFSET);
   len_s = dtls_ec_key_asn1_from_uint32(point_s, DTLS_EC_KEY_SIZE, p + S_KEY_OFFSET(len_r));

#undef R_KEY_OFFSET
#undef S_KEY_OFFSET

   /* sha256 */
   dtls_int_to_uint8(p, TLS_EXT_SIG_HASH_ALGO_SHA256);
   p += sizeof(uint8);

   /* ecdsa */
   dtls_int_to_uint8(p, TLS_EXT_SIG_HASH_ALGO_ECDSA);
   p += sizeof(uint8);

   /* length of signature */
   dtls_int_to_uint16(p, len_r + len_s + 2);
   p += sizeof(uint16);

   /* ASN.1 SEQUENCE */
   dtls_int_to_uint8(p, 0x30);
   p += sizeof(uint8);

   dtls_int_to_uint8(p, len_r + len_s);
   p += sizeof(uint8);

   /* ASN.1 Integer r */

   /* the point r ASN.1 integer was added here so skip */
   p += len_r;

   /* ASN.1 Integer s */

   /* the point s ASN.1 integer was added here so skip */
   p += len_s;

   return p;
}
#endif /* DTLS_ECC */

int appl_settings_get_provisioning(char *buf, size_t len)
{
   int index = 0;
   int start = 0;
   size_t out_len = 0;
   bool sh_prot = sh_protected();

   k_mutex_lock(&settings_mutex, K_FOREVER);

#ifdef CONFIG_PROVISIONING_GROUP
   index += snprintf(buf, len, "%s=%s", device_id, CONFIG_PROVISIONING_GROUP);
#else
   index += snprintf(buf, len, "%s=Auto", device_id);
#endif

   printk("%s", buf);
   buf[index++] = '\n';

#if defined(DTLS_PSK)
   if (psk_key_length && psk_id_length) {
      if (sh_prot) {
         printk("# for PSK provisioning, 'unlock' first!");
      } else {
         start = index;
         index += snprintf(buf + index, len - index, ".psk='%s',", psk_id);
         if (!base64_encode(buf + index, len - index, &out_len, psk_key, psk_key_length)) {
            index += out_len;
            printk("%s", &buf[start]);
            buf[index++] = '\n';
         } else {
            // reset
            index = start;
         }
      }
   }
#endif /* DTLS_PSK */

#if defined(DTLS_ECC)
   if (!is_zero(ecdsa_priv_key, sizeof(ecdsa_priv_key)) &&
       !is_zero(ecdsa_pub_key, sizeof(ecdsa_pub_key))) {
      int start = 0;
      size_t dom_len = 0;
      size_t temp_data_len = 0;
      uint8_t dl;
      uint32_t point_r[9];
      uint32_t point_s[9];
      dtls_hash_ctx data;
      unsigned char sha256hash[DTLS_HMAC_DIGEST_SIZE];
      uint8_t temp_data[sizeof(ecdsa_pub_cert_asn1_header) + DTLS_EC_KEY_SIZE * 2];

      dtls_hash_init(&data);

#ifdef CONFIG_DTLS_PROVISIONING_DOMAIN
      dom_len = strlen(CONFIG_DTLS_PROVISIONING_DOMAIN);
      if (dom_len > 64) {
         dom_len = 0;
      }
#endif
      dl = dom_len;
      dtls_hash_update(&data, &dl, sizeof(dl));
#ifdef CONFIG_DTLS_PROVISIONING_DOMAIN
      if (dom_len) {
         dtls_hash_update(&data, CONFIG_DTLS_PROVISIONING_DOMAIN, dom_len);
         index += snprintf(buf, len, ".dom=%s", CONFIG_DTLS_PROVISIONING_DOMAIN);
         printk("%s", buf);
         buf[index++] = '\n';
      }
#endif

      temp_data_len = sizeof(ecdsa_pub_cert_asn1_header);
      memcpy(temp_data, ecdsa_pub_cert_asn1_header, temp_data_len);
      memcpy(&temp_data[temp_data_len], ecdsa_pub_key, sizeof(ecdsa_pub_key));
      temp_data_len += sizeof(ecdsa_pub_key);

      start = index;
      index += snprintf(buf + index, len - index, ".rpk=");
      if (!base64_encode(buf + index, len - index, &out_len, temp_data, temp_data_len)) {
         dtls_hash_update(&data, temp_data, temp_data_len);
         index += out_len;
         printk("%s", &buf[start]);
         buf[index++] = '\n';
         dtls_hash_finalize(sha256hash, &data);

         dtls_ecdsa_create_sig_hash(ecdsa_priv_key, sizeof(ecdsa_priv_key), sha256hash,
                                    sizeof(sha256hash), point_r, point_s);
         temp_data_len = appl_settings_add_ecdsa_signature_elem(temp_data, point_r, point_s) - temp_data;

         start = index;
         index += snprintf(buf + index, len - index, dom_len ? ".sigdom=" : ".sig=");
         if (!base64_encode(buf + index, len - index, &out_len, temp_data, temp_data_len)) {
            index += out_len;
            printk("%s", &buf[start]);
            buf[index++] = '\n';
         } else {
            // reset
            index = start;
         }
      } else {
         // reset
         index = start;
      }
   }
#endif /* DTLS_ECC */
   k_mutex_unlock(&settings_mutex);
   return index;
}

bool appl_settings_is_provisioning(void)
{
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
   bool res;
   k_mutex_lock(&settings_mutex, K_FOREVER);
   res = ecdsa_provisioning_enabled;
   k_mutex_unlock(&settings_mutex);
   return res;
#else  /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
   return false;
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
}

void appl_settings_provisioning_done(void)
{
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
   k_mutex_lock(&settings_mutex, K_FOREVER);
   if (ecdsa_provisioning_enabled) {
      ecdsa_provisioning_enabled = false;
      settings_save_one(SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_PROV, &ecdsa_provisioning_enabled, sizeof(ecdsa_provisioning_enabled));
   }
   k_mutex_unlock(&settings_mutex);
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
}

bool appl_settings_unlock(const char *value)
{
#ifdef CONFIG_SH_CMD_UNLOCK
   int res = 1;
   if (strlen(value)) {
      k_mutex_lock(&settings_mutex, K_FOREVER);
      res = strcmp(unlock_password, value);
      k_mutex_unlock(&settings_mutex);
   }
   return res == 0;
#else  /* CONFIG_SH_CMD_UNLOCK */
   return false;
#endif /* CONFIG_SH_CMD_UNLOCK */
}

#ifdef CONFIG_SH_CMD

static void appl_settings_expand_key(char *key, size_t max_len)
{
   if (strchr(key, SETTINGS_NAME_SEPARATOR) == NULL) {
      size_t len = strlen(key);
      size_t offset = sizeof(SETTINGS_SERVICE_NAME);
      if (len + offset + 1 <= max_len) {
         memmove(key + offset, key, len + 1);
         memmove(key, SETTINGS_SERVICE_NAME, offset - 1);
         key[offset - 1] = SETTINGS_NAME_SEPARATOR;
      }
   }
}

#ifdef CONFIG_SETTINGS_DEBUG

static int sh_cmd_settings_load(const char *parameter)
{
   ARG_UNUSED(parameter);
   int res = settings_load();
   if (res) {
      LOG_WRN("Settings load: fail (err %d, %s)\n", res, strerror(-res));
   } else {
      LOG_INF("Settings loaded.");
   }
   return res;
}

static int sh_cmd_settings_save(const char *parameter)
{
   ARG_UNUSED(parameter);
   int res = settings_save();
   if (res) {
      LOG_WRN("Settings save: fail (err %d, %s)\n", res, strerror(-res));
   } else {
      LOG_INF("Settings saved.");
   }
   return res;
}

SH_CMD(load, NULL, "settings load.", sh_cmd_settings_load, NULL, 0);
SH_CMD(save, NULL, "settings save.", sh_cmd_settings_save, NULL, 0);

#endif /* CONFIG_SETTINGS_DEBUG */

static int sh_cmd_settings_get(const char *parameter)
{
   int res = 1;
   const char *cur = parameter;
   char key[MAX_SETTINGS_KEY_LENGTH + 1];
   char value[SETTINGS_MAX_VAL_LEN];

   memset(key, 0, sizeof(key));
   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', key, sizeof(key));
   if (!key[0]) {
      res = -EINVAL;
   } else {
      appl_settings_expand_key(key, sizeof(key));
      res = settings_runtime_get(key, (void *)value, sizeof(value));
      if (res < 0) {
         LOG_WRN("Settings get: fail (err %d, %s)\n", res, strerror(-res));
      } else {
         LOG_INF("Get: '%s' %d bytes", key, res);
         LOG_HEXDUMP_INF((void *)value, res, "");
      }
   }

   return res;
}

static void sh_cmd_settings_get_help(void)
{
   LOG_INF("> help get:");
   LOG_INF("  get <key>  : get value for key.");
}

static int sh_cmd_settings_prov(const char *parameter)
{
   ARG_UNUSED(parameter);
   char buf[350];
   if (appl_settings_is_provisioning()) {
      LOG_INF("Auto-provisioning pending.");
   }
   appl_settings_get_provisioning(buf, sizeof(buf));
   return 0;
}

SH_CMD(get, NULL, "get settings.", sh_cmd_settings_get, sh_cmd_settings_get_help, 0);
SH_CMD(prov, NULL, "show provisioning data.", sh_cmd_settings_prov, NULL, 0);

static int sh_cmd_settings_set(const char *parameter)
{
   int res = 1;
   const char *cur = parameter;
   char key[MAX_SETTINGS_KEY_LENGTH + 1];

   memset(key, 0, sizeof(key));
   cur = parse_next_text(cur, ' ', key, sizeof(key));
   if (!key[0] || !*cur) {
      res = -EINVAL;
   } else {
      int len = 0;
      const void *value = NULL;
      char *t = NULL;
      uint16_t val16 = 0;
      uint8_t val8 = 0;

      appl_settings_expand_key(key, sizeof(key));
      if (!strcmp(key, SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_BATTERY_PROFILE)) {
         val8 = strtol(cur, &t, 0);
         value = &val8;
         len = sizeof(val8);
      } else if (!strcmp(key, SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_PORT) ||
                 !strcmp(key, SETTINGS_SERVICE_NAME "/" SETTINGS_KEY_SECURE_PORT)) {
         val16 = strtol(cur, &t, 0);
         value = &val16;
         len = sizeof(val16);
      } else {
         value = cur;
         len = strlen(cur);
      }

      res = settings_runtime_set(key, value, len);
      if (!res) {
         res = settings_save_one(key, value, len);
      }
      if (res < 0) {
         LOG_WRN("Settings set: fail (err %d, %s)\n", res, strerror(-res));
      } else {
         LOG_INF("Set: '%s' := '%s'", key, cur);
      }
   }
   return res;
}

static void sh_cmd_settings_set_help(void)
{
   LOG_INF("> help set:");
   LOG_INF("  set <key> <value>    : set value to key.");
}

static int sh_cmd_settings_sethex(const char *parameter)
{
   int res = 1;
   int len = 1;
   const char *cur = parameter;
   char key[MAX_SETTINGS_KEY_LENGTH + 1];
   uint8_t value[SETTINGS_MAX_VAL_LEN];

   memset(key, 0, sizeof(key));
   cur = parse_next_text(cur, ' ', key, sizeof(key));
   if (!key[0] || !*cur) {
      res = -EINVAL;
   } else {
      appl_settings_expand_key(key, sizeof(key));
      len = hex2bin(cur, strlen(cur), value, sizeof(value));
      res = settings_runtime_set(key, value, len);
      if (!res) {
         res = settings_save_one(key, value, len);
      }
      if (res < 0) {
         LOG_WRN("Settings set: fail (err %d, %s)\n", res, strerror(-res));
      } else {
         LOG_INF("Set: '%s'", key);
         LOG_HEXDUMP_INF(value, res, "   ");
      }
   }
   return res;
}

static void sh_cmd_settings_sethex_help(void)
{
   LOG_INF("> help sethex:");
   LOG_INF("  set <key> <hex-value> : set hexadecimal value for key.");
}

static int sh_cmd_settings_del(const char *parameter)
{
   int res = 1;
   const char *cur = parameter;
   char key[MAX_SETTINGS_KEY_LENGTH + 1];

   memset(key, 0, sizeof(key));
   cur = parse_next_text(cur, ' ', key, sizeof(key));
   if (!key[0]) {
      res = -EINVAL;
   } else {
      appl_settings_expand_key(key, sizeof(key));
      res = settings_runtime_set(key, NULL, 0);
      if (!res) {
         res = settings_delete(key);
      }
      if (res < 0) {
         LOG_WRN("Settings delete: fail (err %d, %s)\n", res, strerror(-res));
      } else {
         LOG_INF("Del: '%s'", key);
      }
   }

   return res;
}

static void sh_cmd_settings_del_help(void)
{
   LOG_INF("> help del:");
   LOG_INF("  del <key>  : delete value for key.");
}

static int sh_cmd_settings_provdone(const char *parameter)
{
   ARG_UNUSED(parameter);
   appl_settings_provisioning_done();
   return 0;
}

SH_CMD(set, NULL, "set settings from text.", sh_cmd_settings_set, sh_cmd_settings_set_help, 1);
SH_CMD(sethex, NULL, "set settings from hexadezimal.", sh_cmd_settings_sethex, sh_cmd_settings_sethex_help, 1);
SH_CMD(del, NULL, "delete settings.", sh_cmd_settings_del, sh_cmd_settings_del_help, 1);
SH_CMD(provdone, NULL, "provisioning done.", sh_cmd_settings_provdone, NULL, 1);

#ifdef DTLS_PSK
static int sh_cmd_settings_generate_psk(const char *parameter)
{
   ARG_UNUSED(parameter);

   k_mutex_lock(&settings_mutex, K_FOREVER);
   if (psk_id_length == 0) {
      LOG_INF("psk_id missing! Provide it before saving.");
   } else {
      LOG_INF("psk-id: %s", psk_id);
   }
   memset(psk_key, 0, sizeof(psk_key));

   dtls_prng(psk_key, 12);
   psk_key_length = 12;
   LOG_INF("psk-secret: %d", psk_key_length);
   LOG_HEXDUMP_INF(psk_key, psk_key_length, "psk:");
   k_mutex_unlock(&settings_mutex);

   return 0;
}

SH_CMD(genpsk, NULL, "generate psk secret.", sh_cmd_settings_generate_psk, NULL, 1);
#endif /* DTLS_PSK */

#ifdef DTLS_ECC
static int sh_cmd_settings_generate_ec(const char *parameter)
{
   ARG_UNUSED(parameter);

   k_mutex_lock(&settings_mutex, K_FOREVER);
   memset(ecdsa_priv_key, 0, sizeof(ecdsa_priv_key));
   memset(ecdsa_pub_key, 0, sizeof(ecdsa_pub_key));

   if (dtls_ecdsa_generate_key2(ecdsa_priv_key, ecdsa_pub_key, DTLS_EC_KEY_SIZE, TLS_EXT_ELLIPTIC_CURVES_SECP256R1)) {
      LOG_INF("ecdsa private key: generated.");
      LOG_HEXDUMP_INF(ecdsa_pub_key, sizeof(ecdsa_pub_key), "generated device public key:");
   } else {
      LOG_INF("ecdsa private key: failed to generate, disabled.");
   }
   k_mutex_unlock(&settings_mutex);

   return 0;
}

SH_CMD(genec, NULL, "generate ec keypair.", sh_cmd_settings_generate_ec, NULL, 1);
#endif

#endif /* CONFIG_SH_CMD */

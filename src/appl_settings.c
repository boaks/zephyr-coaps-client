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

#define DEFAUL_COAP_SERVER "californium.eclipseprojects.io"
#define DEFAUL_COAP_SERVER_PORT 5683
#define DEFAUL_COAP_SERVER_SECURE_PORT 5684

static uint8_t settings_initialized = 0;

static char destination[64] = DEFAUL_COAP_SERVER;
static uint16_t destination_port = DEFAUL_COAP_SERVER_PORT;
static uint16_t destination_secure_port = DEFAUL_COAP_SERVER_SECURE_PORT;
static unsigned char device_imei[DTLS_PSK_MAX_CLIENT_IDENTITY_LEN + 1] = {0};
static unsigned char psk_id[DTLS_PSK_MAX_CLIENT_IDENTITY_LEN + 1] = {0};
static size_t psk_id_length = 0;

#if defined(CONFIG_DTLS_PSK_SECRET) || defined(CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY) || defined(CONFIG_DTLS_ECDSA_PRIVATE_KEY) || defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY)

static int appl_decode_value(const char *desc, const char *value, uint8_t *buf, size_t len)
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
static unsigned char psk_key[DTLS_PSK_MAX_KEY_LEN] = {0};
static size_t psk_key_length = 0;

/* This function is the "key store" for tinyDTLS. It is called to
 * retrieve a key for the given identity within this particular
 * session. */
static int
get_psk_info(dtls_context_t *ctx,
             const session_t *session,
             dtls_credentials_type_t type,
             const unsigned char *id, size_t id_len,
             unsigned char *result, size_t result_length)
{
   ARG_UNUSED(ctx);
   ARG_UNUSED(session);

   switch (type) {
      case DTLS_PSK_IDENTITY:
         if (id_len) {
            LOG_DBG("got psk_identity_hint: '%.*s'", (int)id_len, id);
         }

         if (result_length < psk_id_length) {
            LOG_WRN("cannot set psk_identity -- buffer too small");
            return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
         }

         memcpy(result, psk_id, psk_id_length);
         return psk_id_length;
      case DTLS_PSK_KEY:
         if (id_len != psk_id_length || memcmp(psk_id, id, id_len) != 0) {
            LOG_WRN("PSK for unknown id requested, exiting.");
            return dtls_alert_fatal_create(DTLS_ALERT_ILLEGAL_PARAMETER);
         } else if (result_length < psk_key_length) {
            LOG_WRN("cannot set psk -- buffer too small.");
            return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
         }

         memcpy(result, psk_key, psk_key_length);
         return psk_key_length;
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

static int appl_decode_public_key(const char *desc, const char *value, uint8_t *buf, size_t len)
{
   int res = 0;
   unsigned char temp[sizeof(ecdsa_pub_cert_asn1_header) + DTLS_EC_KEY_SIZE * 2];

   memset(temp, 0, sizeof(temp));
   memset(buf, 0, len);

   if (len >= DTLS_EC_KEY_SIZE * 2) {
      res = appl_decode_value(desc, value, temp, sizeof(temp));

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

static int appl_decode_private_key(const char *desc, const char *value, uint8_t *buf, size_t len)
{
   int res = 0;
   unsigned char temp[sizeof(ecdsa_priv_asn1_header) + DTLS_EC_KEY_SIZE];

   memset(temp, 0, sizeof(temp));
   memset(buf, 0, len);

   if (len >= DTLS_EC_KEY_SIZE) {
      res = appl_decode_value(desc, value, temp, sizeof(temp));

      if (res == sizeof(temp)) {
         if (memcmp(temp, ecdsa_priv_asn1_header, sizeof(ecdsa_priv_asn1_header))) {
            LOG_INF("%s: no SECP256R1 ASN.1 private key.", desc);
         } else {
            res = DTLS_EC_KEY_SIZE;
            memmove(buf, &temp[sizeof(ecdsa_priv_asn1_header)], res);
            sprintf(temp, "%s (from ASN.1):", desc);
            LOG_HEXDUMP_INF(buf, res, temp);
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
get_ecdsa_key(dtls_context_t *ctx,
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
verify_ecdsa_key(dtls_context_t *ctx,
                 const session_t *session,
                 const unsigned char *other_pub_x,
                 const unsigned char *other_pub_y,
                 size_t key_size)
{
   ARG_UNUSED(ctx);
   ARG_UNUSED(session);
   if (key_size != DTLS_EC_KEY_SIZE) {
      return DTLS_ALERT_UNSUPPORTED_CERTIFICATE;
   }
   if (memcmp(trusted_pub_key, other_pub_x, key_size)) {
      return DTLS_ALERT_CERTIFICATE_UNKNOWN;
   }
   if (memcmp(&trusted_pub_key[DTLS_EC_KEY_SIZE], other_pub_y, key_size)) {
      return DTLS_ALERT_CERTIFICATE_UNKNOWN;
   }
   return 0;
}
#endif /* DTLS_ECC */

static int cloud_service_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                    void *cb_arg)
{
   int res = 0;
   const char *next;
   size_t name_len = settings_name_next(name, &next);

   LOG_INF("set: '%s'", name);

   if (!next) {
      if (!strncmp(name, "init", name_len)) {
         res = read_cb(cb_arg, &settings_initialized, sizeof(settings_initialized));
         if (res > 0) {
            LOG_INF("init: %u", settings_initialized);
         }
         return 0;
      }
      if (!strncmp(name, "dest", name_len)) {
         memset(destination, 0, sizeof(destination));
         res = read_cb(cb_arg, destination, sizeof(destination) - 1);
         if (res > 0) {
            LOG_INF("dest: '%s'", destination);
         }
         return 0;
      }

      if (!strncmp(name, "port", name_len)) {
         res = read_cb(cb_arg, &destination_port, sizeof(destination_port));
         if (res > 0) {
            LOG_INF("port: %u", destination_port);
         }
         return 0;
      }
      if (!strncmp(name, "sport", name_len)) {
         res = read_cb(cb_arg, &destination_secure_port, sizeof(destination_secure_port));
         if (res > 0) {
            LOG_INF("secure port: %u", destination_secure_port);
         }
         return 0;
      }

      if (!strncmp(name, "psk_id", name_len)) {
         psk_id_length = 0;
         memset(psk_id, 0, sizeof(psk_id));
         res = read_cb(cb_arg, psk_id, sizeof(psk_id) - 1);
         if (res > 0) {
            LOG_INF("psk_id: '%s'", psk_id);
            psk_id_length = res;
         }
         return 0;
      }

      if (!strncmp(name, "psk_key", name_len)) {
#ifdef DTLS_PSK
         psk_key_length = 0;
         memset(psk_key, 0, sizeof(psk_key));
         res = read_cb(cb_arg, psk_key, sizeof(psk_key));
         if (res > 0) {
            LOG_INF("psk_key: %d bytes", res);
            LOG_HEXDUMP_INF(psk_key, res, "psk:");
            psk_key_length = res;
         }
#endif /* DTLS_PSK */
         return 0;
      }

      if (!strncmp(name, "ec_priv", name_len)) {
#ifdef DTLS_ECC
         memset(ecdsa_priv_key, 0, sizeof(ecdsa_priv_key));
         memset(ecdsa_pub_key, 0, sizeof(ecdsa_pub_key));
         res = read_cb(cb_arg, ecdsa_priv_key, sizeof(ecdsa_priv_key));
         if (res > 0) {
            dtls_ecdsa_generate_public_key2(ecdsa_priv_key, ecdsa_pub_key, DTLS_EC_KEY_SIZE, TLS_EXT_ELLIPTIC_CURVES_SECP256R1);
            if (is_zero(ecdsa_priv_key, sizeof(ecdsa_priv_key))) {
               LOG_INF("ecdsa_priv_key: zero");
            } else {
               LOG_INF("ecdsa_priv_key: %d bytes", res);
            }
         }
#endif /* DTLS_ECC */
         return 0;
      }

      if (!strncmp(name, "tr_pub", name_len)) {
#ifdef DTLS_ECC
         memset(trusted_pub_key, 0, sizeof(trusted_pub_key));
         res = read_cb(cb_arg, trusted_pub_key, sizeof(trusted_pub_key));
         if (res > 0) {
            if (is_zero(trusted_pub_key, sizeof(trusted_pub_key))) {
               LOG_INF("trusted_pub_key: zero");
            } else {
               LOG_INF("trusted_pub_key: %d bytes", res);
            }
         }
#endif /* DTLS_ECC */
         return 0;
      }
      if (!strncmp(name, "ec_prov", name_len)) {
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
         res = read_cb(cb_arg, &ecdsa_provisioning_enabled, sizeof(ecdsa_provisioning_enabled));
         if (res > 0) {
            LOG_INF("provisioning: %u", ecdsa_provisioning_enabled);
         }
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
         return 0;
      }
   }
   LOG_INF("set: '%s' unknown", name);

   return -ENOENT;
}

static int cloud_service_handle_export(int (*cb)(const char *name,
                                                 const void *value, size_t val_len))
{
   LOG_INF("export <" SETTINGS_SERVICE_NAME ">\n");
   (void)cb(SETTINGS_SERVICE_NAME "/init", &settings_initialized, sizeof(settings_initialized));
   (void)cb(SETTINGS_SERVICE_NAME "/port", &destination_port, sizeof(destination_port));
   (void)cb(SETTINGS_SERVICE_NAME "/sport", &destination_secure_port, sizeof(destination_secure_port));
   (void)cb(SETTINGS_SERVICE_NAME "/dest", destination, strlen(destination));
   (void)cb(SETTINGS_SERVICE_NAME "/psk_id", psk_id, psk_id_length);
#ifdef DTLS_PSK
   (void)cb(SETTINGS_SERVICE_NAME "/psk_key", psk_key, psk_key_length);
#endif /* DTLS_PSK */
#ifdef DTLS_ECC
   (void)cb(SETTINGS_SERVICE_NAME "/ec_priv", ecdsa_priv_key, sizeof(ecdsa_priv_key));
   (void)cb(SETTINGS_SERVICE_NAME "/tr_pub", trusted_pub_key, sizeof(trusted_pub_key));
#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
   (void)cb(SETTINGS_SERVICE_NAME "/ec_prov", &ecdsa_provisioning_enabled, sizeof(ecdsa_provisioning_enabled));
#endif
#endif /* DTLS_ECC */
   return 0;
}

static int cloud_service_handle_commit(void)
{
   LOG_INF("loading <" SETTINGS_SERVICE_NAME "> is done\n");
   return 0;
}

static int cloud_service_handle_get(const char *name, char *val, int val_len_max)
{
   int res = 0;
   const char *next;
   size_t name_len = settings_name_next(name, &next);

   LOG_INF("get: '%s'", name);
   name_len = settings_name_next(name, &next);
   memset(val, 0, val_len_max);

   if (!next) {
      if (!strncmp(name, "init", name_len)) {
         res = sizeof(settings_initialized);
         if (res > val_len_max) {
            return -EINVAL;
         }
         memmove(val, &settings_initialized, res);
         LOG_DBG("init: %u", settings_initialized);
         return res;
      }

      if (!strncmp(name, "dest", name_len)) {
         res = strlen(destination) + 1;
         if (res > val_len_max) {
            return -EINVAL;
         }
         memmove(val, destination, res);
         LOG_DBG("dest: '%s'", val);
         return res - 1;
      }

      if (!strncmp(name, "port", name_len)) {
         res = sizeof(destination_port);
         if (res > val_len_max) {
            return -EINVAL;
         }
         memmove(val, &destination_port, res);
         LOG_DBG("port: %u", destination_port);
         return res;
      }

      if (!strncmp(name, "sport", name_len)) {
         res = sizeof(destination_secure_port);
         if (res > val_len_max) {
            return -EINVAL;
         }
         memmove(val, &destination_secure_port, res);
         LOG_DBG("secure port: %u", destination_secure_port);
         return res;
      }

      if (!strncmp(name, "psk_id", name_len)) {
         res = psk_id_length + 1;
         if (res > val_len_max) {
            return -EINVAL;
         }
         memmove(val, psk_id, res);
         LOG_DBG("psk_id: '%s'", val);
         return res - 1;
      }

      if (!strncmp(name, "psk_key", name_len)) {
#ifdef DTLS_PSK
         if (psk_key_length) {
            LOG_INF("Get: '%s' protected!", name);
         } else {
            LOG_DBG("Get: '%s' %d bytes", name, res);
         }
         return res;
#else  /* DTLS_PSK */
         return 0;
#endif /* DTLS_PSK */
      }

      if (!strncmp(name, "ec_priv", name_len)) {
#ifdef DTLS_ECC
         if (!is_zero(ecdsa_priv_key, sizeof(ecdsa_priv_key))) {
            LOG_INF("Get: '%s' protected!", name);
         } else {
            LOG_DBG("Get: '%s' %d bytes", name, res);
         }
         return res;
#else  /* DTLS_ECC */
         return 0;
#endif /* DTLS_ECC */
      }

      if (!strncmp(name, "ec_pub", name_len)) {
#ifdef DTLS_ECC
         if (!is_zero(ecdsa_pub_key, sizeof(ecdsa_pub_key))) {
            res = sizeof(ecdsa_pub_key);
            if (res > val_len_max) {
               return -EINVAL;
            }
            memmove(val, ecdsa_pub_key, res);
         }
         LOG_DBG("Get: '%s' %d bytes", name, res);
         LOG_HEXDUMP_DBG(val, res, "ecdsa_pub");
         return res;
#else  /* DTLS_ECC */
         return 0;
#endif /* DTLS_ECC */
      }

      if (!strncmp(name, "tr_pub", name_len)) {
#ifdef DTLS_ECC
         if (!is_zero(trusted_pub_key, sizeof(trusted_pub_key))) {
            res = sizeof(trusted_pub_key);
            if (res > val_len_max) {
               return -EINVAL;
            }
            memmove(val, trusted_pub_key, res);
         }
         LOG_DBG("Get: '%s' %d bytes", name, res);
         LOG_HEXDUMP_DBG(val, res, "trusted_pub");
         return res;
#else  /* DTLS_ECC */
         return 0;
#endif /* DTLS_ECC */
      }

      if (!strncmp(name, "ec_prov", name_len)) {
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
         res = sizeof(ecdsa_provisioning_enabled);
         if (res > val_len_max) {
            return -EINVAL;
         }
         memmove(val, &ecdsa_provisioning_enabled, res);
         LOG_DBG("provisioning: %u", ecdsa_provisioning_enabled);
         return res;
#else  /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
         return 0;
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
      }
   }
   LOG_WRN("get: '%s' unknown", name);

   return -ENOENT;
}

/* static subtree handler */
SETTINGS_STATIC_HANDLER_DEFINE(cloud_service, SETTINGS_SERVICE_NAME, cloud_service_handle_get,
                               cloud_service_handle_set, cloud_service_handle_commit,
                               cloud_service_handle_export);

static int appl_settings_init(void)
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

SYS_INIT(appl_settings_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#define SETTINGS_RESET_DEST 1
#define SETTINGS_RESET_PSK 2
#define SETTINGS_RESET_ECDSA 4
#define SETTINGS_RESET_TRUST 8
#define SETTINGS_RESET_PROVISIONING 16

#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
static int settings_init_provisioning(void)
{
   size_t len;
   memset(ecdsa_provisioning_priv_key, 0, sizeof(ecdsa_provisioning_priv_key));
   memset(ecdsa_provisioning_pub_key, 0, sizeof(ecdsa_provisioning_pub_key));
   len = appl_decode_private_key("ecdsa provisioning private key", CONFIG_DTLS_ECDSA_AUTO_PROVISIONING_PRIVATE_KEY, ecdsa_provisioning_priv_key, sizeof(ecdsa_provisioning_priv_key));
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

static void setting_factory_reset(int flags)
{
   bool save = false;
   int res = 0;

   if (flags & SETTINGS_RESET_DEST) {
      memset(destination, 0, sizeof(destination));
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
      save = true;
   }

   if (flags & SETTINGS_RESET_PSK) {
      unsigned char *cur;
      size_t id_len;

      memset(psk_id, 0, sizeof(psk_id));
#ifdef CONFIG_DTLS_PSK_IDENTITY
      strncpy(psk_id, CONFIG_DTLS_PSK_IDENTITY, sizeof(psk_id) - 1);
#endif /* CONFIG_DTLS_PSK_IDENTITY */
      id_len = strlen(psk_id);
      cur = (unsigned char *)strstr(psk_id, "${imei}");
      if (cur) {
         size_t imei_len = strlen(device_imei);
         if (imei_len && (imei_len + id_len - 6) < sizeof(psk_id)) {
            memmove(cur + imei_len, cur + 7, id_len - 6 - (cur - psk_id));
            memmove(cur, device_imei, imei_len);
         } else {
            uint32_t id = 0;
            dtls_prng((unsigned char *)&id, sizeof(id));
            snprintf(cur, sizeof(psk_id) - (cur - psk_id), "%u", id);
         }
      }
      LOG_INF("psk-id: %s", psk_id);
      psk_id_length = strlen(psk_id);
#ifdef DTLS_PSK
#ifdef CONFIG_DTLS_PSK_SECRET_GENERATE
      dtls_prng(psk_key, 12);
      res = 12;
#elif defined(CONFIG_DTLS_PSK_SECRET)
      res = appl_decode_value("psk-secret", CONFIG_DTLS_PSK_SECRET, psk_key, sizeof(psk_key));
#else
      res = 0;
#endif
      if (res > 0) {
         psk_key_length = res;
         LOG_INF("psk-secret: %d", res);
         LOG_HEXDUMP_INF(psk_key, psk_key_length, "psk:");
      } else {
         psk_key_length = 0;
         LOG_INF("no psk-secret, disabled");
      }
#endif /* DTLS_PSK */
      save = true;
   }

#ifdef DTLS_ECC
   if (flags & SETTINGS_RESET_ECDSA) {
      memset(ecdsa_priv_key, 0, sizeof(ecdsa_priv_key));
      memset(ecdsa_pub_key, 0, sizeof(ecdsa_pub_key));

#ifdef CONFIG_DTLS_ECDSA_PRIVATE_KEY_GENERATE
      if (dtls_ecdsa_generate_key2(ecdsa_priv_key, ecdsa_pub_key, DTLS_EC_KEY_SIZE, TLS_EXT_ELLIPTIC_CURVES_SECP256R1)) {
         LOG_INF("ecdsa private key: generated.");
         LOG_HEXDUMP_INF(ecdsa_pub_key, sizeof(ecdsa_pub_key), "generated device public key:");
      } else {
         LOG_INF("ecdsa private key: failed to generate, disbaled.");
      }
#elif defined(CONFIG_DTLS_ECDSA_PRIVATE_KEY)
      res = appl_decode_private_key("ecdsa private key", CONFIG_DTLS_ECDSA_PRIVATE_KEY, ecdsa_priv_key, sizeof(ecdsa_priv_key));
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
      if (settings_init_provisioning()) {
         ecdsa_provisioning_enabled = 1;
         LOG_INF("ecdsa provisioning enabled.");
         save = true;
      }
   }
#endif /* CONFIG_DTLS_ECDSA_PROVISIONING */

   if (flags & SETTINGS_RESET_TRUST) {
      memset(trusted_pub_key, 0, sizeof(trusted_pub_key));
#ifdef CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY
      res = appl_decode_public_key("trusted public key", CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY, trusted_pub_key, sizeof(trusted_pub_key));
#else
      res = 0;
#endif /* CONFIG_DTLS_ECDSA_TRUSTED_PUBLIC_KEY */
      if (is_zero(trusted_pub_key, sizeof(trusted_pub_key))) {
         LOG_INF("ecdsa no trusted public key: disabled.");
      }
      save = true;
   }
#endif /* DTLS_ECC */

   if (save || !settings_initialized) {
      settings_initialized = 1;
      settings_save();
   }
}

void dtls_settings_init(const char *imei, dtls_handler_t *handler)
{
   if (imei) {
      memset(device_imei, 0, sizeof(device_imei));
      strncpy(device_imei, imei, sizeof(device_imei) - 1);
   }

   if (settings_initialized) {
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
      if (handler && ecdsa_provisioning_enabled) {
         if (!settings_init_provisioning()) {
            dtls_provisioning_done();
         }
      }
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
   } else {
      if (handler) {
         setting_factory_reset(SETTINGS_RESET_DEST | SETTINGS_RESET_PSK | SETTINGS_RESET_ECDSA | SETTINGS_RESET_TRUST | SETTINGS_RESET_PROVISIONING);
      } else {
         setting_factory_reset(SETTINGS_RESET_DEST | SETTINGS_RESET_PSK);
      }
   }

   if (handler) {

#ifdef DTLS_PSK
      if (psk_id_length && psk_key_length) {
         handler->get_psk_info = get_psk_info;
         LOG_INF("Enable PSK");
      }
#endif /* DTLS_PSK */
#ifdef DTLS_ECC
      if (!is_zero(ecdsa_priv_key, sizeof(ecdsa_priv_key)) &&
          !is_zero(ecdsa_pub_key, sizeof(ecdsa_pub_key))) {
         handler->get_ecdsa_key = get_ecdsa_key;
         LOG_INF("Enable ECDSA");
      }
      if (!is_zero(trusted_pub_key, sizeof(trusted_pub_key))) {
         handler->verify_ecdsa_key = verify_ecdsa_key;
         LOG_INF("Enable ECDSA trust");
      }
#endif /* DTLS_ECC */
   }
}

const char *dtls_get_destination(void)
{
   return destination;
}

const char *dtls_get_psk_identity(void)
{
   return psk_id;
}

#if defined(DTLS_ECC)

static uint8 *
dtls_add_ecdsa_signature_elem(uint8 *p, uint32_t *point_r, uint32_t *point_s)
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

int dtls_get_provisioning(char *buf, size_t len)
{
   int index = 0;
   int start = 0;
   size_t out_len = 0;

#ifdef CONFIG_DTLS_PROVISIONING_GROUP
   index += snprintf(buf, len, "%s=%s", psk_id, CONFIG_DTLS_PROVISIONING_GROUP);
#else
   index += snprintf(buf, len, "%s=Auto", psk_id);
#endif
   printk("%s", buf);
   buf[index++] = '\n';

   if (psk_key_length) {
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
         temp_data_len = dtls_add_ecdsa_signature_elem(temp_data, point_r, point_s) - temp_data;

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
#endif
   return index;
}

uint16_t dtls_get_destination_port(bool secure)
{
   return secure ? destination_secure_port : destination_port;
}

bool dtls_is_provisioning(void)
{
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
   return ecdsa_provisioning_enabled;
#else  /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
   return false;
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
}

void dtls_provisioning_done(void)
{
#if defined(DTLS_ECC) && defined(CONFIG_DTLS_ECDSA_AUTO_PROVISIONING)
   if (ecdsa_provisioning_enabled) {
      ecdsa_provisioning_enabled = false;
      settings_save_one(SETTINGS_SERVICE_NAME "/ec_prov", &ecdsa_provisioning_enabled, sizeof(ecdsa_provisioning_enabled));
   }
#endif /* DTLS_ECC && CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
}

#ifdef CONFIG_SH_CMD

static void settings_expand_key(char *key, size_t max_len)
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
      settings_expand_key(key, sizeof(key));
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
      settings_expand_key(key, sizeof(key));
      res = settings_runtime_set(key, cur, strlen(cur));
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
      settings_expand_key(key, sizeof(key));
      len = hex2bin(cur, strlen(cur), value, sizeof(value));
      res = settings_runtime_set(key, value, len);
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
   LOG_INF("  set <key> <hex-value>    : set hexadecimal value for key.");
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
      settings_expand_key(key, sizeof(key));
      res = settings_delete(key);
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

static int sh_cmd_settings_prov(const char *parameter)
{
   ARG_UNUSED(parameter);
   char buf[350];
   if (dtls_is_provisioning()) {
      LOG_INF("Auto-provisioning pending.");
   }
   dtls_get_provisioning(buf, sizeof(buf));
   return 0;
}

static int sh_cmd_settings_provdone(const char *parameter)
{
   ARG_UNUSED(parameter);
   dtls_provisioning_done();
   return 0;
}

SH_CMD(get, NULL, "get settings.", sh_cmd_settings_get, sh_cmd_settings_get_help, 0);
SH_CMD(set, NULL, "set settings from text.", sh_cmd_settings_set, sh_cmd_settings_set_help, 0);
SH_CMD(sethex, NULL, "set settings from hexadezimal.", sh_cmd_settings_sethex, sh_cmd_settings_sethex_help, 0);
SH_CMD(del, NULL, "delete settings.", sh_cmd_settings_del, sh_cmd_settings_del_help, 0);
SH_CMD(load, NULL, "settings load.", sh_cmd_settings_load, NULL, 0);
SH_CMD(save, NULL, "settings save.", sh_cmd_settings_save, NULL, 0);
SH_CMD(prov, NULL, "show provisioning data.", sh_cmd_settings_prov, NULL, 0);
SH_CMD(provdone, NULL, "provisioning done.", sh_cmd_settings_provdone, NULL, 0);

#endif /* CONFIG_SH_CMD */

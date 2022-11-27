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

#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>
#include <modem/nrf_modem_lib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include "appl_settings.h"
#include "coap_appl_client.h"
#include "modem.h"
#include "tcp_client.h"
#include "ui.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define HTTP_PORT 80
#define HTTPS_PORT 443

#define STR(S) #S
#define STRSTR(S) STR(S)

/* Certificate for `californium.eclipseprojects.io` */
static const char coap_cert[] = {
#include CONFIG_COAP_SERVER_TRUST_CERTIFICATE
};

static const char http_cert[] = {
#include CONFIG_HTTP_SERVER_TRUST_CERTIFICATE
};

#define HTTP_HEAD        \
   "HEAD / HTTP/1.1\r\n" \
   "Host: %s:%u\r\n"     \
   "Connection: %s\r\n\r\n"

#define COAP_TLS_SEC_TAG 42
#define HTTP_TLS_SEC_TAG 43

#define LED_CONNECT LED_NONE

static int fd = -1;
static int connected = 0;

BUILD_ASSERT(sizeof(coap_cert) < KB(4), "CoAP Certificate too large");
BUILD_ASSERT(sizeof(http_cert) < KB(4), "HTTP Certificate too large");

static int cert_provision(const char *name, nrf_sec_tag_t sec_tag, const void *buf, size_t len)
{
   bool exists;
   int err;
   int mismatch;

   /* It may be sufficient for you application to check whether the correct
    * certificate is provisioned with a given tag directly using modem_key_mgmt_cmp().
    * Here, for the sake of the completeness, we check that a certificate exists
    * before comparing it with what we expect it to be.
    */
   err = modem_key_mgmt_exists(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);
   if (err) {
      LOG_ERR("Failed to check for %s certificates err %d (%s)", name, err, strerror(-err));
      return err;
   }

   if (exists) {
      mismatch = modem_key_mgmt_cmp(sec_tag,
                                    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                    buf, len);
      if (!mismatch) {
         LOG_INF("%s certificate match", name);
         return err;
      }

      LOG_INF("%s certificate mismatch", name);
      err = modem_key_mgmt_delete(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
      if (err) {
         LOG_ERR("Failed to delete existing %s certificate, err %d (%s)", name, err, strerror(-err));
      }
   }

   LOG_INF("Provisioning %s certificate", name);

   /*  Provision certificate to the modem */
   err = modem_key_mgmt_write(sec_tag,
                              MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                              buf, len);
   if (err) {
      LOG_ERR("Failed to provision %s certificate, err %d (%s)", name, err, strerror(-err));
   }
   return err;
}

/* Provision certificate to modem */
int tls_cert_provision(void)
{
   cert_provision("coap", COAP_TLS_SEC_TAG,  coap_cert, strlen(coap_cert));
   return cert_provision("http", HTTP_TLS_SEC_TAG,  http_cert, strlen(http_cert));
}

/* Setup TLS options on a given socket */
static int tls_setup(int fd)
{
   int err;
   int value;
   char destination[MAX_SETTINGS_VALUE_LENGTH];

   appl_settings_get_destination(destination, sizeof(destination));

   /* Security tag that we have provisioned the certificate with */
   const sec_tag_t tls_sec_tag[] = {
       COAP_TLS_SEC_TAG,
       HTTP_TLS_SEC_TAG,
   };

#if defined(CONFIG_SAMPLE_TFM_MBEDTLS)
   err = tls_credential_add(tls_sec_tag[0], TLS_CREDENTIAL_CA_CERTIFICATE, coap_cert, sizeof(coap_cert));
   err = tls_credential_add(tls_sec_tag[1], TLS_CREDENTIAL_CA_CERTIFICATE, http_cert, sizeof(http_cert));
   if (err) {
      return err;
   }
#endif

   value = TLS_PEER_VERIFY_REQUIRED;
   err = setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &value, sizeof(value));
   if (err < 0) {
      LOG_ERR("Failed to setup peer verification, err %d\n", errno);
      return err;
   }

   /* Associate the socket with the security tag
    * we have provisioned the certificate with.
    */
   err = setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, tls_sec_tag,
                    sizeof(tls_sec_tag));
   if (err < 0) {
      LOG_ERR("Failed to setup TLS sec tag, err %d\n", errno);
      return err;
   }

   err = setsockopt(fd, SOL_TLS, TLS_HOSTNAME, destination, strlen(destination));
   if (err < 0) {
      LOG_ERR("Failed to setup TLS hostname, err %d\n", errno);
      return err;
   }

#if 0
   value = TLS_SESSION_CACHE_ENABLED;
   err = setsockopt(fd, SOL_TLS, TLS_SESSION_CACHE, &value, sizeof(value));
   if (err < 0) {
      LOG_ERR("Failed to setup session cache, err %d\n", errno);
      return err;
   }
   LOG_INF("Enabled session cache.\n");
#endif

   return 0;
}

static void tcp_close(void)
{
   if (fd > -1) {
      (void)close(fd);
      fd = -1;
      connected = 0;
   }
}

static int tcp_open(int tls)
{
   int err = 0;

   if (tls) {
      if (IS_ENABLED(CONFIG_SAMPLE_TFM_MBEDTLS)) {
         fd = socket(AF_INET, SOCK_STREAM | SOCK_NATIVE_TLS, IPPROTO_TLS_1_2);
      } else {
         fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
      }
   } else {
      fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   }
   if (fd < 0) {
      LOG_ERR("Failed to open socket!\n");
      err = -1;
      goto clean_up;
   }
   connected = 0;

   if (tls) {
      /* Setup TLS socket options */
      err = tls_setup(fd);
      if (err < 0) {
         goto clean_up;
      }
   }
#ifdef CONFIG_UDP_AS_RAI_ENABLE
   LOG_INF("RAI ongoing\n");
   if (setsockopt(fd, SOL_SOCKET, SO_RAI_ONGOING, NULL, 0)) {
      LOG_ERR("RAI error %d\n", errno);
   }
#endif
   return err;
clean_up:
   tcp_close();
   return err;
}

static int tcp_send(int fd, const uint8_t *buffer, size_t len)
{
   int result = 0;
   int offset = 0;
   if (len > 0) {
      do {
         result = send(fd, &buffer[offset], len - offset, 0);
         if (result < 0) {
            return result;
         }
         offset += result;
      } while (offset < len);
   }
   return offset;
}

static int tcp_recv(int fd, uint8_t *buffer, size_t length)
{
   int bytes = 0;
   size_t offset = 0;

   memset(buffer, 0, length);
   do {
      bytes = recv(fd, &buffer[offset], length - offset, 0);
      if (bytes < 0) {
         return bytes;
      }
      offset += bytes;
   } while (offset < length && bytes != 0 /* == 0, peer closed connection */);

   return offset;
}

int http_head(session_t *dst, bool tls, bool keep_connection, unsigned long *connected_time, uint8_t *buffer, size_t len)
{
   const char *protocol = tls ? "https" : "http";
   const unsigned int port = ntohs(dst->addr.sin.sin_port);
   char destination[MAX_SETTINGS_VALUE_LENGTH];

   appl_settings_get_destination(destination, sizeof(destination));

   int header = snprintf(buffer, len, HTTP_HEAD, destination, port, keep_connection ? "keep-alive" : "close");
   int result = 0;
   char *p;

   if (fd < 0) {
      result = tcp_open(tls);
      if (result < 0) {
         goto clean_up;
      }
   }

   LOG_INF("%s client HEAD %d bytes\n\r", protocol, header);

   LOG_INF("Connecting to %s:%u\n", destination, port);
   LOG_INF("======================\n");
   LOG_INF("%s", buffer);
   LOG_INF("======================\n");

   if (!connected) {
      result = connect(fd, &dst->addr.sa, sizeof(struct sockaddr));
      if (result < 0) {
         LOG_ERR("%s connect() failed, err: %d\n", protocol, errno);
         goto clean_up;
      }
      connected = 1;
      if (connected_time) {
         *connected_time = (unsigned long)k_uptime_get();
      }
   }
   ui_led_op(LED_COLOR_GREEN, LED_SET);
   ui_led_op(LED_CONNECT, LED_SET);

   result = tcp_send(fd, buffer, header);
   if (result < 0) {
      LOG_ERR("%s send() failed, err %d\n", protocol, errno);
      goto clean_up;
   }

#if 0
   LOG_INF("RAI no data (%d)\n", SO_RAI_NO_DATA);
   if (setsockopt(fd, SOL_SOCKET, SO_RAI_NO_DATA, NULL, 0)) {
      LOG_ERR("RAI error %d\n", errno);
   }
#endif

   LOG_INF("%s sent %d bytes\n", protocol, result);

   memset(buffer, 0, len);
   result = tcp_recv(fd, buffer, len);
   if (result < 0) {
      LOG_ERR("%s recv() failed, err %d\n", protocol, errno);
      goto clean_up;
   }

   LOG_INF("%s received %d bytes\n", protocol, result);

   /* Make sure recv_buf is NULL terminated (for safe use with strstr) */
   if (result < len) {
      buffer[result] = '\0';
   } else {
      buffer[len - 1] = '\0';
   }

   /* Print HTTP response */
   p = strstr(buffer, "\r\n\r\n");
   if (p) {
      *p = 0;
      LOG_INF("\n>\t %s\n\n", buffer);
   }

   if (keep_connection) {
      LOG_INF("Finished.\n");
   } else {
      LOG_INF("Finished, closing socket.\n");
      tcp_close();
      ui_led_op(LED_CONNECT, LED_CLEAR);
   }
   return result;

clean_up:
   tcp_close();
   ui_led_op(LED_CONNECT, LED_CLEAR);
   return result;
}

static int tcp_coap_decode_length(const uint8_t *buffer, size_t length)
{
   if (length == 2 && buffer[0] == 0) {
      return 2;
   } else if (length > 2) {
      LOG_INF("recv() 0x%02x 0x%02x 0x%02x\n", buffer[0], buffer[1], buffer[2]);
      int token_len = buffer[0] & 0xf;
      int length_type = (buffer[0] >> 4) & 0xf;
      if (length_type < 13) {
         return length_type + token_len + 2;
      } else if (length_type == 13) {
         return (buffer[1] & 0xff) + token_len + 13 + 3;
      } else if (length_type == 14) {
         return ((buffer[1] & 0xff) << 8) +
                (buffer[2] & 0xff) +
                token_len + 13 + 256 + 4;
      } else if (length_type == 15 && length > 3) {
         return ((buffer[1] & 0xff) << 16) +
                ((buffer[2] & 0xff) << 8) +
                (buffer[3] & 0xff) +
                token_len + 13 + 256 + 65536 + 5;
      }
   }
   return -1;
}

static int tcp_coap_client_prepare_post(uint8_t *coap_message_buf, uint16_t coap_message_len)
{
   uint16_t message_len;
   uint8_t token_len;
   uint8_t code;

   token_len = coap_message_buf[0] & 0xf;
   code = coap_message_buf[1];
   message_len = coap_message_len - token_len - 4;

   if (message_len < 13) {
      coap_message_buf[0] = token_len | (message_len << 4);
      memmove(&coap_message_buf[2], &coap_message_buf[4], coap_message_len - 4);
      coap_message_len -= 2;
   } else if (message_len < 13 + 256) {
      coap_message_buf[0] = token_len | (13 << 4);
      coap_message_buf[1] = message_len - 13;
      coap_message_buf[2] = code;
      memmove(&coap_message_buf[3], &coap_message_buf[4], coap_message_len - 4);
      --coap_message_len;
   } else {
      coap_message_buf[0] = token_len | (14 << 4);
      message_len -= (13 + 256);
      coap_message_buf[1] = (message_len >> 8);
      coap_message_buf[2] = message_len;
      coap_message_buf[3] = code;
   }

   return coap_message_len;
}

static int tcp_coap_client_prepare_response(uint8_t *buffer, size_t length, size_t max_length)
{
   if (buffer && length > 1) {
      int code = 0;
      int token_len = buffer[0] & 0xf;
      int length_type = (buffer[0] >> 4) & 0xf;
      int coap_length = -1;

      if (length_type < 13) {
         coap_length = length_type + token_len + 4;
         if ((length + 2) != coap_length || coap_length > max_length) {
            // length error
            return -1;
         }
         code = buffer[1] & 0xff;
         memmove(&buffer[4], &buffer[2], length - 2);
      } else if (length_type == 13) {
         coap_length = (buffer[1] & 0xff) + token_len + 4 + 13;
         if ((length + 1) != coap_length || coap_length > max_length) {
            // length error
            return -1;
         }
         code = buffer[2] & 0xff;
         memmove(&buffer[4], &buffer[3], length - 3);
      } else if (length_type == 14) {
         coap_length = ((buffer[1] & 0xff) << 8) +
                       (buffer[2] & 0xff) +
                       token_len + 4 + 13 + 256;
         if (length != coap_length || coap_length > max_length) {
            // length error
            return -1;
         }
         code = buffer[3] & 0xff;
      } else if (length_type == 15) {
         coap_length = ((buffer[1] & 0xff) << 16) +
                       ((buffer[2] & 0xff) << 8) +
                       (buffer[3] & 0xff) + token_len + 4 + 13 + 256 + 65536;
         if ((length - 1) != coap_length || coap_length > max_length) {
            // length error
            return -1;
         }
         code = buffer[4] & 0xff;
         memmove(&buffer[4], &buffer[5], length - 5);
      }
      buffer[0] = token_len + 0x50; // NON
      buffer[1] = code;
      buffer[2] = 0;
      buffer[3] = 0;
      return coap_length;
   }
   return -1;
}

static int tcp_recv_coap(int fd, uint8_t *buffer, size_t length)
{
   int bytes = 0;
   int coap_message_length = -1;
   size_t offset = 0;

   memset(buffer, 0, length);
   do {
      bytes = recv(fd, &buffer[offset], length - offset, 0);
      if (bytes < 0) {
         return bytes;
      }
      offset += bytes;
      if (coap_message_length < 0) {
         coap_message_length = tcp_coap_decode_length(buffer, bytes);
         if (coap_message_length >= 0 && coap_message_length < length) {
            length = coap_message_length;
         }
      }
   } while (offset < length && bytes != 0 /* == 0, peer closed connection */);

   return offset;
}

int coap_post(session_t *dst, bool tls, bool keep_connection, unsigned long *connected_time, uint8_t *buffer, size_t len)
{
   const char *protocol = tls ? "coaps+tcp" : "coap+tcp";
   const unsigned int port = ntohs(dst->addr.sin.sin_port);
   size_t coap_buffer_len = 0;
   const uint8_t *coap_buffer = NULL;
   char destination[MAX_SETTINGS_VALUE_LENGTH];

   appl_settings_get_destination(destination, sizeof(destination));

   int result = coap_appl_client_prepare_post(buffer, len, COAP_SEND_FLAGS);

   if (result < 0) {
      return result;
   }
   coap_buffer_len = coap_appl_client_message(&coap_buffer);
   memmove(buffer, coap_buffer, coap_buffer_len);

   result = tcp_coap_client_prepare_post(buffer, coap_buffer_len);
   if (result < 0) {
      return result;
   }
   coap_buffer_len = result;

   LOG_INF("%s client POST %d bytes\n\r", protocol, coap_buffer_len);

   if (fd < 0) {
      result = tcp_open(tls);
      if (result < 0) {
         goto clean_up;
      }
   }
   ui_led_op(LED_COLOR_GREEN, LED_SET);

   LOG_INF("Connecting to %s:%u\n", destination, port);

   if (!connected) {
      result = connect(fd, &dst->addr.sa, sizeof(struct sockaddr));
      if (result < 0) {
         LOG_ERR("%s connect() failed, err: %d\n", protocol, errno);
         goto clean_up;
      }
      connected = 1;
      if (connected_time) {
         *connected_time = (unsigned long)k_uptime_get();
      }
   }
   ui_led_op(LED_CONNECT, LED_SET);

   result = tcp_send(fd, buffer, coap_buffer_len);
   if (result < 0) {
      LOG_ERR("%s send() failed, err %d\n", protocol, errno);
      goto clean_up;
   }

   LOG_INF("%s sent %d bytes\n", protocol, result);

   result = tcp_recv_coap(fd, buffer, len);
   if (result < 0) {
      LOG_ERR("%s recv() failed, err: %d\n", protocol, errno);
      goto clean_up;
   }
   LOG_INF("%s received %d bytes\n", protocol, result);

   result = tcp_coap_client_prepare_response(buffer, result, len);
   result = coap_appl_client_parse_data(buffer, result);

   if (keep_connection) {
      LOG_INF("Finished.\n");
   } else {
      LOG_INF("Finished, closing socket.\n");
      tcp_close();
      ui_led_op(LED_CONNECT, LED_CLEAR);
   }
   modem_set_transmission_time();
   return result;

clean_up:
   tcp_close();
   ui_led_op(LED_CONNECT, LED_CLEAR);
   return result;
}

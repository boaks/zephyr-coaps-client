/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>
#include <modem/nrf_modem_lib.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include "modem.h"

#define HTTPS_PORT 443

#define HTTPS_HOSTNAME "example.com"

#define HTTP_HEAD                     \
   "HEAD / HTTP/1.1\r\n"              \
   "Host: " HTTPS_HOSTNAME ":443\r\n" \
   "Connection: close\r\n\r\n"

#define HTTP_HEAD_LEN (sizeof(HTTP_HEAD) - 1)

#define HTTP_HDR_END "\r\n\r\n"

#define RECV_BUF_SIZE 2048
#define TLS_SEC_TAG 42

static const char send_buf[] = HTTP_HEAD;
static char recv_buf[RECV_BUF_SIZE];
static struct addrinfo *res;
static int fd = -1;

/* Certificate for `example.com` */
static const char cert[] = {
#include "../cert/DigiCertGlobalRootCA.pem"
};

BUILD_ASSERT(sizeof(cert) < KB(4), "Certificate too large");

/* Provision certificate to modem */
int https_cert_provision(void)
{
   int err;
   bool exists;
   int mismatch;

   /* It may be sufficient for you application to check whether the correct
    * certificate is provisioned with a given tag directly using modem_key_mgmt_cmp().
    * Here, for the sake of the completeness, we check that a certificate exists
    * before comparing it with what we expect it to be.
    */
   err = modem_key_mgmt_exists(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);
   if (err) {
      printk("Failed to check for certificates err %d\n", err);
      return err;
   }

   if (exists) {
      mismatch = modem_key_mgmt_cmp(TLS_SEC_TAG,
                                    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                    cert, strlen(cert));
      if (!mismatch) {
         printk("Certificate match\n");
         return 0;
      }

      printk("Certificate mismatch\n");
      err = modem_key_mgmt_delete(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
      if (err) {
         printk("Failed to delete existing certificate, err %d\n", err);
      }
   }

   printk("Provisioning certificate\n");

   /*  Provision certificate to the modem */
   err = modem_key_mgmt_write(TLS_SEC_TAG,
                              MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                              cert, sizeof(cert) - 1);
   if (err) {
      printk("Failed to provision certificate, err %d\n", err);
      return err;
   }

   return 0;
}

/* Setup TLS options on a given socket */
static int tls_setup(int fd)
{
   int err;
   int value;

   /* Security tag that we have provisioned the certificate with */
   const sec_tag_t tls_sec_tag[] = {
       TLS_SEC_TAG,
   };

#if defined(CONFIG_SAMPLE_TFM_MBEDTLS)
   err = tls_credential_add(tls_sec_tag[0], TLS_CREDENTIAL_CA_CERTIFICATE, cert, sizeof(cert));
   if (err) {
      return err;
   }
#endif

   value = TLS_PEER_VERIFY_REQUIRED;
   err = setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &value, sizeof(value));
   if (err) {
      printk("Failed to setup peer verification, err %d\n", errno);
      return err;
   }

   /* Associate the socket with the security tag
    * we have provisioned the certificate with.
    */
   err = setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, tls_sec_tag,
                    sizeof(tls_sec_tag));
   if (err) {
      printk("Failed to setup TLS sec tag, err %d\n", errno);
      return err;
   }

   err = setsockopt(fd, SOL_TLS, TLS_HOSTNAME, HTTPS_HOSTNAME, sizeof(HTTPS_HOSTNAME) - 1);
   if (err) {
      printk("Failed to setup TLS hostname, err %d\n", errno);
      return err;
   }

#if 0
   /* experimental! */
   value = TLS_SESSION_CACHE_ENABLED;
   err = setsockopt(fd, SOL_TLS, TLS_SESSION_CACHE, &value, sizeof(value));
   if (err) {
      printk("Failed to setup session cache, err %d\n", errno);
      return err;
   }
#endif

   return 0;
}

static void https_close(void)
{
   if (res) {
      freeaddrinfo(res);
   }
   if (fd > -1) {
      (void)close(fd);
      fd = -1;
   }
}

int https_init_destination(void)
{
   int err;
   struct addrinfo hints = {
       .ai_family = AF_INET,
       .ai_socktype = SOCK_STREAM,
   };

   err = getaddrinfo(HTTPS_HOSTNAME, NULL, &hints, &res);
   if (err) {
      printk("getaddrinfo() failed, err %d\n", errno);
      return err;
   }

   ((struct sockaddr_in *)res->ai_addr)->sin_port = htons(HTTPS_PORT);

   return 0;
}

int https_open(void)
{
   int err;

   if (IS_ENABLED(CONFIG_SAMPLE_TFM_MBEDTLS)) {
      fd = socket(AF_INET, SOCK_STREAM | SOCK_NATIVE_TLS, IPPROTO_TLS_1_2);
   } else {
      fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
   }
   if (fd == -1) {
      printk("Failed to open socket!\n");
      err = -1;
      goto clean_up;
   }

   /* Setup TLS socket options */
   err = tls_setup(fd);
   if (err) {
      goto clean_up;
   }
   return err;
clean_up:
   https_close();
   return err;
}

int https_get(void)
{
   int err;
   char *p;
   int bytes;
   size_t off;

   printk("HTTPS client GET\n\r");

   if (fd < 0) {
      https_open();
   }

   printk("Connecting to %s\n", HTTPS_HOSTNAME);
   err = connect(fd, res->ai_addr, sizeof(struct sockaddr_in));
   if (err) {
      printk("connect() failed, err: %d\n", errno);
      goto clean_up;
   }

   off = 0;
   do {
      bytes = send(fd, &send_buf[off], HTTP_HEAD_LEN - off, 0);
      if (bytes < 0) {
         printk("send() failed, err %d\n", errno);
         err = bytes;
         goto clean_up;
      }
      off += bytes;
   } while (off < HTTP_HEAD_LEN);

   printk("Sent %d bytes\n", off);

   off = 0;
   do {
      bytes = recv(fd, &recv_buf[off], RECV_BUF_SIZE - off, 0);
      if (bytes < 0) {
         printk("recv() failed, err %d\n", errno);
         err = bytes;
         goto clean_up;
      }
      off += bytes;
   } while (bytes != 0 /* peer closed connection */);

   printk("Received %d bytes\n", off);

   /* Make sure recv_buf is NULL terminated (for safe use with strstr) */
   if (off < sizeof(recv_buf)) {
      recv_buf[off] = '\0';
   } else {
      recv_buf[sizeof(recv_buf) - 1] = '\0';
   }

   /* Print HTTP response */
   p = strstr(recv_buf, "\r\n");
   if (p) {
      off = p - recv_buf;
      recv_buf[off + 1] = '\0';
      printk("\n>\t %s\n\n", recv_buf);
   }

   printk("Finished, closing socket.\n");
   (void)close(fd);
   fd = -1;
   return err;

clean_up:
   https_close();
   return err;
}

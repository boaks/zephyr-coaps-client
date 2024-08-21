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

#if defined(CONFIG_NRF_MODEM_LIB)
#include <modem/lte_lc.h>

#include "modem_desc.h"

const char *modem_get_system_mode_description(
    enum lte_lc_system_mode lte_mode,
    enum lte_lc_system_mode_preference lte_preference)
{
   switch (lte_mode) {
      case LTE_LC_SYSTEM_MODE_LTEM:
         return "LTE-M";
      case LTE_LC_SYSTEM_MODE_NBIOT:
         return "NB-IoT";
      case LTE_LC_SYSTEM_MODE_GPS:
         return "GPS";
      case LTE_LC_SYSTEM_MODE_LTEM_GPS:
         return "LTE-M/GPS";
      case LTE_LC_SYSTEM_MODE_NBIOT_GPS:
         return "NB-IoT/GPS";
      case LTE_LC_SYSTEM_MODE_LTEM_NBIOT:
         switch (lte_preference) {
            case LTE_LC_SYSTEM_MODE_PREFER_AUTO:
               return "LTE-M/NB-IoT (auto)";
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM:
               return "LTE-M/NB-IoT";
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT:
               return "NB-IoT/LTE-M";
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM_PLMN_PRIO:
               return "LTE-M/NB-IoT (plmn)";
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT_PLMN_PRIO:
               return "NB-IoT/LTE-M (plmn)";
         }
         return "LTE-M/NB-IoT (\?\?\?)";
      case LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS:
         switch (lte_preference) {
            case LTE_LC_SYSTEM_MODE_PREFER_AUTO:
               return "LTE-M/NB-IoT/GPS (auto)";
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM:
               return "LTE-M/NB-IoT/GPS";
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT:
               return "NB-IoT/LTE-M/GPS";
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM_PLMN_PRIO:
               return "LTE-M/NB-IoT/GPS (plmn)";
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT_PLMN_PRIO:
               return "NB-IoT/LTE-M/GPS (plmn)";
         }
         return "LTE-M/NB-IoT/GPS (\?\?\?)";
   }
   return "LTE \?\?\?";
}

const char *modem_get_system_mode_cfg(
    enum lte_lc_system_mode lte_mode,
    enum lte_lc_system_mode_preference lte_preference)
{
   switch (lte_mode) {
      case LTE_LC_SYSTEM_MODE_LTEM:
         return "m1";
      case LTE_LC_SYSTEM_MODE_NBIOT:
         return "nb";
      case LTE_LC_SYSTEM_MODE_GPS:
         return "(GPS)";
      case LTE_LC_SYSTEM_MODE_LTEM_GPS:
         return "m1 (GPS)";
      case LTE_LC_SYSTEM_MODE_NBIOT_GPS:
         return "nb (GPS)";
      case LTE_LC_SYSTEM_MODE_LTEM_NBIOT:
         switch (lte_preference) {
            case LTE_LC_SYSTEM_MODE_PREFER_AUTO:
               return "m1 nb (no pref.)";
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM:
               return "m1 nb";
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT:
               return "nb m1";
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM_PLMN_PRIO:
               return "m1 nb (plmn prio)";
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT_PLMN_PRIO:
               return "nb m1 (plmn prio)";
         }
         return "m1 nb (\?\?\?)";
      case LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS:
         switch (lte_preference) {
            case LTE_LC_SYSTEM_MODE_PREFER_AUTO:
               return "m1 nb (GPS, no pref.)";
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM:
               return "m1 nb (GPS)";
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT:
               return "nb m1 (GPS)";
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM_PLMN_PRIO:
               return "m1 nb (GPS, plmn prio)";
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT_PLMN_PRIO:
               return "nb m1 (GPS, plmn prio)";
         }
         return "m1 nb (GPS, \?\?\?)";
   }
   return "\?\?\?";
}

const char *modem_get_registration_description(enum lte_lc_nw_reg_status reg_status)
{
   switch (reg_status) {
      case LTE_LC_NW_REG_NOT_REGISTERED:
         return "Not Registered";
      case LTE_LC_NW_REG_REGISTERED_HOME:
         return "Registered - home network";
      case LTE_LC_NW_REG_SEARCHING:
         return "Searching ...";
      case LTE_LC_NW_REG_REGISTRATION_DENIED:
         return "Not Registered - denied";
      case LTE_LC_NW_REG_UNKNOWN:
         break;
      case LTE_LC_NW_REG_REGISTERED_ROAMING:
         return "Registered - roaming network";
      case LTE_LC_NW_REG_UICC_FAIL:
         return "Not Registered - UICC fail";
      default:
         break;
   }
   return "Unknown";
}

const char *modem_get_registration_short_description(enum lte_lc_nw_reg_status reg_status)
{
   switch (reg_status) {
      case LTE_LC_NW_REG_NOT_REGISTERED:
         return "not reg.";
      case LTE_LC_NW_REG_REGISTERED_HOME:
         return "home";
      case LTE_LC_NW_REG_SEARCHING:
         return "search";
      case LTE_LC_NW_REG_REGISTRATION_DENIED:
         return "denied";
      case LTE_LC_NW_REG_UNKNOWN:
         break;
      case LTE_LC_NW_REG_REGISTERED_ROAMING:
         return "roaming";
      case LTE_LC_NW_REG_UICC_FAIL:
         return "UICC fail";
      default:
         break;
   }
   return "unknown";
}

const char *modem_get_network_mode_description(enum lte_lc_lte_mode mode)
{
   switch (mode) {
      case LTE_LC_LTE_MODE_NONE:
         return "none";
      case LTE_LC_LTE_MODE_LTEM:
         return "CAT-M1";
      case LTE_LC_LTE_MODE_NBIOT:
         return "NB-IoT";
   }
   return "Unknown";
}

const char *modem_get_rai_description(enum lte_network_rai rai)
{
   switch (rai) {
      case LTE_NETWORK_NO_RAI:
         return "no RAI";
      case LTE_NETWORK_CP_RAI:
         return "CP-RAI";
      case LTE_NETWORK_AS_RAI:
         return "AS-RAI";
      default:
         break;
   }
   return "Unknown";
}

const char *modem_get_state_type(enum lte_network_state_type state_type)
{
   switch (state_type) {
      case LTE_NETWORK_STATE_INIT:
         return "init";
      case LTE_NETWORK_STATE_OFF:
         return "off";
      case LTE_NETWORK_STATE_ON:
         return "on";
      default:
         break;
   }
   return "Unknown";
}

const char *modem_get_emm_cause_description(int cause)
{
   switch (cause) {
      case 2:
         return "IMSI unknown";
      case 3:
         return "UE illegal";
      case 5:
         return "IMEI not accepted";
      case 6:
         return "ME illegal";
      case 7:
         return "EPS not allowed";
      case 8:
         return "EPS and non-EPS not allowed";
      case 9:
         return "UE unknown";
      case 10:
         return "Detached";
      case 11:
         return "PLMN not allowed";
      case 12:
         return "TAC not allowed";
      case 13:
         return "Roaming in TAC not allowed";
      case 14:
         return "EPS in PLMN not allowed";
      case 15:
         return "No suitable cells in TAC";
      case 16:
         return "MSC temporary not reachable";
      case 17:
         return "Network failure";
      case 18:
         return "CS domain not available";
      case 19:
         return "ESM failure";
      case 20:
         return "MAC failure";
      case 21:
         return "Synch failure";
      case 22:
         return "Congestion";
      case 23:
         return "UE security capabilities mismatch";
      case 24:
         return "Security mode rejected";
      case 25:
         return "Not authorized for CSG";
      case 26:
         return "Non-EPS authentication not accepted";
      case 35:
         return "Service option not authorized for PLMN";
      case 39:
         return "CS temporary not available";
      case 40:
         return "No EPS bearer";
      case 95:
         return "Incorrect message";
      case 96:
         return "Invalid mandatory information";
      case 97:
         return "Message type unknown";
      case 98:
         return "Message type uncompatible";
      case 99:
         return "Information unknown";
      case 100:
         return "Conditional IE error";
      case 101:
         return "Message uncompatible";
      case 111:
         return "Protocol error";
   }
   return NULL;
}

/* TS 36.101 version 14.3.0, page 107 */
static const int earfcn_to_band[] = {
    0, 600, 1200, 1950, 2400, 2650, 2750, 3450, 3800, 4150, 4750,
    5010, 5180, 5280, 5380, 5480, 5730, 5850, 6000, 6150, 6450,
    6600, 7500, 7700, 8040, 8690, 9040, 9210, 9660, -1};

int modem_get_band(int earfcn)
{
   int band = 0;
   while (earfcn_to_band[band] <= earfcn) {
      ++band;
      if (earfcn_to_band[band] < 0) {
         band = 0;
         break;
      }
   }
   return band;
}

#endif

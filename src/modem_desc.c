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
      case LTE_LC_SYSTEM_MODE_NONE:
         return "none";
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
      case LTE_LC_SYSTEM_MODE_NONE:
         return "none";
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
      case LTE_LC_NW_REG_REGISTERED_EMERGENCY:
         return "Registered - emergency network";
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
      case LTE_LC_NW_REG_REGISTERED_EMERGENCY:
         return "emergency";
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

#endif

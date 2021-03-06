/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2012 Red Hat, Inc.
 * Copyright (C) 2011 - 2012 Google Inc.
 * Copyright (C) 2012 Huawei Technologies Co., Ltd
 * Copyright (C) 2012 - 2013 Aleksander Morgado <aleksander@gnu.org>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <gudev/gudev.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-errors-types.h"
#include "mm-modem-helpers.h"
#include "mm-modem-helpers-huawei.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-iface-modem-3gpp-ussd.h"
#include "mm-iface-modem-location.h"
#include "mm-iface-modem-time.h"
#include "mm-iface-modem-cdma.h"
#include "mm-broadband-modem-huawei.h"
#include "mm-broadband-bearer-huawei.h"
#include "mm-broadband-bearer.h"
#include "mm-bearer-list.h"
#include "mm-sim-huawei.h"

static void iface_modem_init (MMIfaceModem *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);
static void iface_modem_3gpp_ussd_init (MMIfaceModem3gppUssd *iface);
static void iface_modem_location_init (MMIfaceModemLocation *iface);
static void iface_modem_cdma_init (MMIfaceModemCdma *iface);
static void iface_modem_time_init (MMIfaceModemTime *iface);

static MMIfaceModem *iface_modem_parent;
static MMIfaceModem3gpp *iface_modem_3gpp_parent;
static MMIfaceModemLocation *iface_modem_location_parent;
static MMIfaceModemCdma *iface_modem_cdma_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemHuawei, mm_broadband_modem_huawei, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP_USSD, iface_modem_3gpp_ussd_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_CDMA, iface_modem_cdma_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_TIME, iface_modem_time_init));

typedef enum {
    FEATURE_SUPPORT_UNKNOWN,
    FEATURE_NOT_SUPPORTED,
    FEATURE_SUPPORTED
} FeatureSupport;

struct _MMBroadbandModemHuaweiPrivate {
    /* Regex for signal quality related notifications */
    GRegex *rssi_regex;
    GRegex *rssilvl_regex;
    GRegex *hrssilvl_regex;

    /* Regex for access-technology related notifications */
    GRegex *mode_regex;

    /* Regex for connection status related notifications */
    GRegex *dsflowrpt_regex;
    GRegex *ndisstat_regex;

    /* Regex to ignore */
    GRegex *boot_regex;
    GRegex *connect_regex;
    GRegex *csnr_regex;
    GRegex *cusatp_regex;
    GRegex *cusatend_regex;
    GRegex *dsdormant_regex;
    GRegex *simst_regex;
    GRegex *srvst_regex;
    GRegex *stin_regex;
    GRegex *hcsq_regex;
    GRegex *pdpdeact_regex;
    GRegex *ndisend_regex;
    GRegex *rfswitch_regex;
    GRegex *position_regex;
    GRegex *posend_regex;

    FeatureSupport ndisdup_support;
    FeatureSupport rfswitch_support;
    FeatureSupport sysinfoex_support;
    FeatureSupport syscfg_support;
    FeatureSupport syscfgex_support;
    FeatureSupport prefmode_support;
    FeatureSupport time_support;
    FeatureSupport nwtime_support;

    MMModemLocationSource enabled_sources;

    GArray *syscfg_supported_modes;
    GArray *syscfgex_supported_modes;
    GArray *prefmode_supported_modes;
};

/*****************************************************************************/

static GList *
get_at_port_list (MMBroadbandModemHuawei *self)
{
    GList *out = NULL;
    MMPortSerialAt *port;
    GList *cdc_wdm_at_ports;

    /* Primary */
    port = mm_base_modem_get_port_primary (MM_BASE_MODEM (self));
    if (port)
        out = g_list_append (out, port);

    /* Secondary */
    port = mm_base_modem_get_port_secondary (MM_BASE_MODEM (self));
    if (port)
        out = g_list_append (out, port);

    /* Additional cdc-wdm ports used for dialing */
    cdc_wdm_at_ports = mm_base_modem_find_ports (MM_BASE_MODEM (self),
                                                 MM_PORT_SUBSYS_USB,
                                                 MM_PORT_TYPE_AT,
                                                 NULL);

    return g_list_concat (out, cdc_wdm_at_ports);
}

/*****************************************************************************/

typedef struct {
    gboolean extended;
    guint srv_status;
    guint srv_domain;
    guint roam_status;
    guint sim_state;
    guint sys_mode;
    gboolean sys_submode_valid;
    guint sys_submode;
} SysinfoResult;

static gboolean
sysinfo_finish (MMBroadbandModemHuawei *self,
                GAsyncResult *res,
                gboolean *extended,
                guint *srv_status,
                guint *srv_domain,
                guint *roam_status,
                guint *sim_state,
                guint *sys_mode,
                gboolean *sys_submode_valid,
                guint *sys_submode,
                GError **error)
{
    SysinfoResult *result;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    result = (SysinfoResult *) g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));

    if (extended)
        *extended = result->extended;
    if (srv_status)
        *srv_status = result->srv_status;
    if (srv_domain)
        *srv_domain = result->srv_domain;
    if (roam_status)
        *roam_status = result->roam_status;
    if (sim_state)
        *sim_state = result->sim_state;
    if (sys_mode)
        *sys_mode = result->sys_mode;
    if (sys_submode_valid)
        *sys_submode_valid = result->sys_submode_valid;
    if (sys_submode)
        *sys_submode = result->sys_submode;

    return TRUE;
}

static void
run_sysinfo_ready (MMBaseModem *self,
                   GAsyncResult *res,
                   GSimpleAsyncResult *simple)
{
    GError *error = NULL;
    const gchar *response;
    SysinfoResult *result;

    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response) {
        mm_dbg ("^SYSINFO failed: %s", error->message);
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    result = g_new0 (SysinfoResult, 1);
    result->extended = FALSE;
    if (!mm_huawei_parse_sysinfo_response (response,
                                           &result->srv_status,
                                           &result->srv_domain,
                                           &result->roam_status,
                                           &result->sys_mode,
                                           &result->sim_state,
                                           &result->sys_submode_valid,
                                           &result->sys_submode,
                                           &error)) {
        mm_dbg ("^SYSINFO parsing failed: %s", error->message);
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        g_free (result);
        return;
    }

    g_simple_async_result_set_op_res_gpointer (simple, result, g_free);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
run_sysinfo (MMBroadbandModemHuawei *self,
             GSimpleAsyncResult *result)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "^SYSINFO",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)run_sysinfo_ready,
                              result);
}

static void
run_sysinfoex_ready (MMBaseModem *_self,
                     GAsyncResult *res,
                     GSimpleAsyncResult *simple)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);
    GError *error = NULL;
    const gchar *response;
    SysinfoResult *result;

    response = mm_base_modem_at_command_finish (_self, res, &error);
    if (!response) {
        /* First time we try, we fallback to ^SYSINFO */
        if (self->priv->sysinfoex_support == FEATURE_SUPPORT_UNKNOWN) {
            self->priv->sysinfoex_support = FEATURE_NOT_SUPPORTED;
            mm_dbg ("^SYSINFOEX failed: %s, assuming unsupported", error->message);
            g_error_free (error);
            run_sysinfo (self, simple);
            return;
        }

        /* Otherwise, propagate error */
        mm_dbg ("^SYSINFOEX failed: %s", error->message);
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    if (self->priv->sysinfoex_support == FEATURE_SUPPORT_UNKNOWN)
        self->priv->sysinfoex_support = FEATURE_SUPPORTED;

    result = g_new0 (SysinfoResult, 1);
    result->extended = TRUE;
    if (!mm_huawei_parse_sysinfoex_response (response,
                                             &result->srv_status,
                                             &result->srv_domain,
                                             &result->roam_status,
                                             &result->sim_state,
                                             &result->sys_mode,
                                             &result->sys_submode,
                                             &error)) {
        mm_dbg ("^SYSINFOEX parsing failed: %s", error->message);
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        g_free (result);
        return;
    }

    /* Submode from SYSINFOEX always valid */
    result->sys_submode_valid = TRUE;
    g_simple_async_result_set_op_res_gpointer (simple, result, g_free);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
run_sysinfoex (MMBroadbandModemHuawei *self,
               GSimpleAsyncResult *result)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "^SYSINFOEX",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)run_sysinfoex_ready,
                              result);
}

static void
sysinfo (MMBroadbandModemHuawei *self,
         GAsyncReadyCallback callback,
         gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        sysinfo);
    if (self->priv->sysinfoex_support == FEATURE_SUPPORT_UNKNOWN ||
        self->priv->sysinfoex_support == FEATURE_SUPPORTED)
        run_sysinfoex (self, result);
    else
        run_sysinfo (self, result);
}

/*****************************************************************************/
/* Reset (Modem interface) */

static gboolean
reset_finish (MMIfaceModem *self,
              GAsyncResult *res,
              GError **error)
{
    return !!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
}

static void
reset (MMIfaceModem *self,
       GAsyncReadyCallback callback,
       gpointer user_data)
{
    const gchar *command;

    /* Unlike other Huawei modems that support AT^RESET for resetting the modem,
     * Huawei MU736 supports AT^RESET but does not reset the modem upon receiving
     * AT^RESET. It does, however, support resetting itself via AT+CFUN=16.
     */
    if (g_strcmp0 (mm_iface_modem_get_model (self), "MU736") == 0)
        command = "+CFUN=16";
    else
        command = "^RESET";

    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              command,
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* Load access technologies (Modem interface) */

static MMModemAccessTechnology
huawei_sysinfo_submode_to_act (guint submode)
{
    /* new more detailed system mode/access technology */
    switch (submode) {
    case 1:
        return MM_MODEM_ACCESS_TECHNOLOGY_GSM;
    case 2:
        return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
    case 3:
        return MM_MODEM_ACCESS_TECHNOLOGY_EDGE;
    case 4:
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    case 5:
        return MM_MODEM_ACCESS_TECHNOLOGY_HSDPA;
    case 6:
        return MM_MODEM_ACCESS_TECHNOLOGY_HSUPA;
    case 7:
        return MM_MODEM_ACCESS_TECHNOLOGY_HSPA;
    case 8: /* TD-SCDMA */
        break;
    case 9:  /* HSPA+ */
        return MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS;
    case 10:
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDO0;
    case 11:
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDOA;
    case 12:
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDOB;
    case 13:  /* 1xRTT */
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case 16:  /* 3xRTT */
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case 17: /* HSPA+ (64QAM) */
        return MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS;
    case 18: /* HSPA+ (MIMO) */
        return MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS;
    default:
        break;
    }

    return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

static MMModemAccessTechnology
huawei_sysinfo_mode_to_act (guint mode)
{
    /* Older, less detailed system mode/access technology */
    switch (mode) {
    case 1:  /* AMPS */
        break;
    case 2:  /* CDMA */
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case 3:  /* GSM/GPRS */
        return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
    case 4:  /* HDR */
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDO0;
    case 5:  /* WCDMA */
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    case 6:  /* GPS */
        break;
    case 7:  /* GSM/WCDMA */
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    case 8:  /* CDMA/HDR hybrid */
        return (MM_MODEM_ACCESS_TECHNOLOGY_EVDO0 | MM_MODEM_ACCESS_TECHNOLOGY_1XRTT);
    case 15: /* TD-SCDMA */
        break;
    default:
        break;
    }

    return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

static MMModemAccessTechnology
huawei_sysinfoex_submode_to_act (guint submode)
{
    switch (submode) {
    case 1: /* GSM */
        return MM_MODEM_ACCESS_TECHNOLOGY_GSM;
    case 2: /* GPRS */
        return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
    case 3: /* EDGE */
        return MM_MODEM_ACCESS_TECHNOLOGY_EDGE;

    case 21: /* IS95A */
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case 22: /* IS95B */
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case 23: /* CDMA2000 1x */
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case 24: /* EVDO rel0 */
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDO0;
    case 25: /* EVDO relA */
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDOA;
    case 26: /* EVDO relB */
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDOB;
    case 27: /* Hybrid CDMA2000 1x */
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case 28: /* Hybrid EVDO rel0 */
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDO0;
    case 29: /* Hybrid EVDO relA */
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDOA;
    case 30: /* Hybrid EVDO relB */
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDOB;

    case 41: /* WCDMA */
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    case 42: /* HSDPA */
        return MM_MODEM_ACCESS_TECHNOLOGY_HSDPA;
    case 43: /* HSUPA */
        return MM_MODEM_ACCESS_TECHNOLOGY_HSUPA;
    case 44: /* HSPA */
        return MM_MODEM_ACCESS_TECHNOLOGY_HSPA;
    case 45: /* HSPA+ */
        return MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS;
    case 46: /* DC-HSPA+ */
        return MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS;

    case 61: /* TD-SCDMA */
        break;

    case 81: /* 802.16e (WiMAX) */
        break;

    case 101: /* LTE */
        return MM_MODEM_ACCESS_TECHNOLOGY_LTE;

    default:
        break;
    }

    return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

static MMModemAccessTechnology
huawei_sysinfoex_mode_to_act (guint mode)
{
    /* Older, less detailed system mode/access technology */
    switch (mode) {
    case 1:  /* GSM */
        return MM_MODEM_ACCESS_TECHNOLOGY_GSM;
    case 2:  /* CDMA */
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case 3: /* WCDMA */
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    case 4: /* TD-SCDMA */
        break;
    case 5: /* WIMAX */
        break;
    case 6:  /* LTE */
        return MM_MODEM_ACCESS_TECHNOLOGY_LTE;
    default:
        break;
    }

    return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

static gboolean
load_access_technologies_finish (MMIfaceModem *self,
                                 GAsyncResult *res,
                                 MMModemAccessTechnology *access_technologies,
                                 guint *mask,
                                 GError **error)
{
    gchar *str;
    MMModemAccessTechnology act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    gboolean extended = FALSE;
    guint srv_status = 0;
    gboolean sys_submode_valid = FALSE;
    guint sys_submode = 0;
    guint sys_mode = 0;

    if (!sysinfo_finish (MM_BROADBAND_MODEM_HUAWEI (self),
                         res,
                         &extended,
                         &srv_status,
                         NULL, /* srv_domain */
                         NULL, /* roam_status */
                         NULL, /* sim_state */
                         &sys_mode,
                         &sys_submode_valid,
                         &sys_submode,
                         error))
        return FALSE;

    if (srv_status != 0) {
        /* Valid service */
        if (sys_submode_valid)
            act = (extended ?
                   huawei_sysinfoex_submode_to_act (sys_submode) :
                   huawei_sysinfo_submode_to_act (sys_submode));

        if (act == MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN)
            act = (extended ?
                   huawei_sysinfoex_mode_to_act (sys_mode) :
                   huawei_sysinfo_mode_to_act (sys_mode));
    }

    str = mm_modem_access_technology_build_string_from_mask (act);
    mm_dbg ("Access Technology: '%s'", str);
    g_free (str);

    *access_technologies = act;
    *mask = MM_MODEM_ACCESS_TECHNOLOGY_ANY;
    return TRUE;
}

static void
load_access_technologies (MMIfaceModem *self,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    mm_dbg ("loading access technology (huawei)...");
    sysinfo (MM_BROADBAND_MODEM_HUAWEI (self), callback, user_data);
}

/*****************************************************************************/
/* Load unlock retries (Modem interface) */

static MMUnlockRetries *
load_unlock_retries_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    MMUnlockRetries *unlock_retries;
    const gchar *result;
    GRegex *r;
    GMatchInfo *match_info = NULL;
    GError *match_error = NULL;
    guint i;
    MMModemLock locks[4] = {
        MM_MODEM_LOCK_SIM_PUK,
        MM_MODEM_LOCK_SIM_PIN,
        MM_MODEM_LOCK_SIM_PUK2,
        MM_MODEM_LOCK_SIM_PIN2
    };

    result = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!result)
        return NULL;

    r = g_regex_new ("\\^CPIN:\\s*([^,]+),[^,]*,(\\d+),(\\d+),(\\d+),(\\d+)",
                     G_REGEX_UNGREEDY, 0, NULL);
    g_assert (r != NULL);

    if (!g_regex_match_full (r, result, strlen (result), 0, 0, &match_info, &match_error)) {
        if (match_error)
            g_propagate_error (error, match_error);
        else
            g_set_error (error,
                         MM_CORE_ERROR,
                         MM_CORE_ERROR_FAILED,
                         "Could not parse ^CPIN results: Response didn't match (%s)",
                         result);

        g_match_info_free (match_info);
        g_regex_unref (r);
        return NULL;
    }

    unlock_retries = mm_unlock_retries_new ();
    for (i = 0; i <= 3; i++) {
        guint num;

        if (!mm_get_uint_from_match_info (match_info, i + 2, &num) ||
            num > 10) {
            g_set_error (error,
                         MM_CORE_ERROR,
                         MM_CORE_ERROR_FAILED,
                         "Could not parse ^CPIN results: "
                         "Missing or invalid match info for lock '%s'",
                         mm_modem_lock_get_string (locks[i]));
            g_object_unref (unlock_retries);
            unlock_retries = NULL;
            break;
        }

        mm_unlock_retries_set (unlock_retries, locks[i], num);
    }

    g_match_info_free (match_info);
    g_regex_unref (r);

    return unlock_retries;
}

static void
load_unlock_retries (MMIfaceModem *self,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    mm_dbg ("loading unlock retries (huawei)...");
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "^CPIN?",
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* After SIM unlock (Modem interface) */

static gboolean
modem_after_sim_unlock_finish (MMIfaceModem *self,
                               GAsyncResult *res,
                               GError **error)
{
    return TRUE;
}

static gboolean
after_sim_unlock_wait_cb (GSimpleAsyncResult *result)
{
    g_simple_async_result_complete (result);
    g_object_unref (result);
    return FALSE;
}

static void
modem_after_sim_unlock (MMIfaceModem *self,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_after_sim_unlock);

    /* A 3-second wait is necessary for SIM to become ready, or the firmware may
     * fail miserably and reboot itself */
    g_timeout_add_seconds (3, (GSourceFunc)after_sim_unlock_wait_cb, result);
}

/*****************************************************************************/
/* Common band/mode handling code */

typedef struct {
    MMModemBand mm;
    guint32 huawei;
} BandTable;

static BandTable bands[] = {
    /* Sort 3G first since it's preferred */
    { MM_MODEM_BAND_U2100, 0x00400000 },
    { MM_MODEM_BAND_U1900, 0x00800000 },
    { MM_MODEM_BAND_U850,  0x04000000 },
    { MM_MODEM_BAND_U900,  0x00020000 },
    { MM_MODEM_BAND_G850,  0x00080000 },
    /* 2G second */
    { MM_MODEM_BAND_DCS,   0x00000080 },
    { MM_MODEM_BAND_EGSM,  0x00000100 },
    { MM_MODEM_BAND_PCS,   0x00200000 }
};

static gboolean
bands_array_to_huawei (GArray *bands_array,
                       guint32 *out_huawei)
{
    guint i;

    /* Treat ANY as a special case: All huawei flags enabled */
    if (bands_array->len == 1 &&
        g_array_index (bands_array, MMModemBand, 0) == MM_MODEM_BAND_ANY) {
        *out_huawei = 0x3FFFFFFF;
        return TRUE;
    }

    *out_huawei = 0;
    for (i = 0; i < bands_array->len; i++) {
        guint j;

        for (j = 0; j < G_N_ELEMENTS (bands); j++) {
            if (g_array_index (bands_array, MMModemBand, i) == bands[j].mm)
                *out_huawei |= bands[j].huawei;
        }
    }

    return (*out_huawei > 0 ? TRUE : FALSE);
}

static gboolean
huawei_to_bands_array (guint32 huawei,
                       GArray **bands_array,
                       GError **error)
{
    guint i;

    *bands_array = NULL;
    for (i = 0; i < G_N_ELEMENTS (bands); i++) {
        if (huawei & bands[i].huawei) {
            if (G_UNLIKELY (!*bands_array))
                *bands_array = g_array_new (FALSE, FALSE, sizeof (MMModemBand));
            g_array_append_val (*bands_array, bands[i].mm);
        }
    }

    if (!*bands_array) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Couldn't build bands array from '%u'",
                     huawei);
        return FALSE;
    }

    return TRUE;
}

static gboolean
parse_syscfg (const gchar *response,
              GArray **bands_array,
              GError **error)
{
    gint mode;
    gint acquisition_order;
    guint32 band;
    gint roaming;
    gint srv_domain;

    if (!response ||
        strncmp (response, "^SYSCFG:", 8) != 0 ||
        !sscanf (response + 8, "%d,%d,%x,%d,%d", &mode, &acquisition_order, &band, &roaming, &srv_domain)) {
        /* Dump error to upper layer */
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unexpected SYSCFG response: '%s'",
                     response);
        return FALSE;
    }

    /* Band */
    if (bands_array &&
        !huawei_to_bands_array (band, bands_array, error))
        return FALSE;

    return TRUE;
}

/*****************************************************************************/
/* Load current bands (Modem interface) */

static GArray *
load_current_bands_finish (MMIfaceModem *self,
                           GAsyncResult *res,
                           GError **error)
{
    const gchar *response;
    GArray *bands_array = NULL;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!response)
        return NULL;

    if (!parse_syscfg (response, &bands_array, error))
        return NULL;

    return bands_array;
}

static void
load_current_bands (MMIfaceModem *self,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    mm_dbg ("loading current bands (huawei)...");
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "^SYSCFG?",
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* Set current bands (Modem interface) */

static gboolean
set_current_bands_finish (MMIfaceModem *self,
                          GAsyncResult *res,
                          GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
syscfg_set_ready (MMBaseModem *self,
                  GAsyncResult *res,
                  GSimpleAsyncResult *operation_result)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error))
        /* Let the error be critical */
        g_simple_async_result_take_error (operation_result, error);
    else
        g_simple_async_result_set_op_res_gboolean (operation_result, TRUE);

    g_simple_async_result_complete (operation_result);
    g_object_unref (operation_result);
}

static void
set_current_bands (MMIfaceModem *self,
                   GArray *bands_array,
                   GAsyncReadyCallback callback,
                   gpointer user_data)
{
    GSimpleAsyncResult *result;
    gchar *cmd;
    guint32 huawei_band = 0x3FFFFFFF;
    gchar *bands_string;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        set_current_bands);

    bands_string = mm_common_build_bands_string ((MMModemBand *)bands_array->data,
                                                 bands_array->len);

    if (!bands_array_to_huawei (bands_array, &huawei_band)) {
        g_simple_async_result_set_error (result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Invalid bands requested: '%s'",
                                         bands_string);
        g_simple_async_result_complete_in_idle (result);
        g_object_unref (result);
        g_free (bands_string);
        return;
    }

    cmd = g_strdup_printf ("AT^SYSCFG=16,3,%X,2,4", huawei_band);
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              cmd,
                              3,
                              FALSE,
                              (GAsyncReadyCallback)syscfg_set_ready,
                              result);
    g_free (cmd);
    g_free (bands_string);
}

/*****************************************************************************/
/* Load supported modes (Modem interface) */

static GArray *
load_supported_modes_finish (MMIfaceModem *self,
                             GAsyncResult *res,
                             GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    return g_array_ref (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
}

static void
syscfg_test_ready (MMBroadbandModemHuawei *self,
                   GAsyncResult *res,
                   GSimpleAsyncResult *simple)
{
    const gchar *response;
    GError *error = NULL;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (response) {
        /* There are 2G+3G Huawei modems out there which support mode switching with
         * AT^SYSCFG, but fail to provide a valid response for AT^SYSCFG=? (they just
         * return an empty string). So handle that case by providing a default response
         * string to get parsed. Ugly, ugly, blame Huawei.
         */
        if (response[0])
            self->priv->syscfg_supported_modes = mm_huawei_parse_syscfg_test (response, &error);
        else {
            self->priv->syscfg_supported_modes = mm_huawei_parse_syscfg_test (MM_HUAWEI_DEFAULT_SYSCFG_FMT, NULL);
            g_assert (self->priv->syscfg_supported_modes != NULL);
        }
    }

    if (self->priv->syscfg_supported_modes) {
        MMModemModeCombination mode;
        guint i;
        GArray *combinations;

        /* Build list of combinations */
        combinations = g_array_sized_new (FALSE,
                                          FALSE,
                                          sizeof (MMModemModeCombination),
                                          self->priv->syscfg_supported_modes->len);
        for (i = 0; i < self->priv->syscfg_supported_modes->len; i++) {
            MMHuaweiSyscfgCombination *huawei_mode;

            huawei_mode = &g_array_index (self->priv->syscfg_supported_modes,
                                          MMHuaweiSyscfgCombination,
                                          i);
            mode.allowed = huawei_mode->allowed;
            mode.preferred = huawei_mode->preferred;
            g_array_append_val (combinations, mode);
        }

        self->priv->syscfg_support = FEATURE_SUPPORTED;
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   combinations,
                                                   (GDestroyNotify)g_array_unref);
    } else {
        g_debug ("Error while checking ^SYSCFG format: %s", error->message);
        /* If SIM-PIN error, don't mark as feature unsupported; we'll retry later */
        if (!g_error_matches (error,
                              MM_MOBILE_EQUIPMENT_ERROR,
                              MM_MOBILE_EQUIPMENT_ERROR_SIM_PIN))
            self->priv->syscfg_support = FEATURE_NOT_SUPPORTED;
        g_simple_async_result_take_error (simple, error);
    }

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
syscfgex_test_ready (MMBroadbandModemHuawei *self,
                     GAsyncResult *res,
                     GSimpleAsyncResult *simple)
{
    const gchar *response;
    GError *error = NULL;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (response)
        self->priv->syscfgex_supported_modes = mm_huawei_parse_syscfgex_test (response, &error);

    if (self->priv->syscfgex_supported_modes) {
        MMModemModeCombination mode;
        guint i;
        GArray *combinations;

        /* Build list of combinations */
        combinations = g_array_sized_new (FALSE,
                                          FALSE,
                                          sizeof (MMModemModeCombination),
                                          self->priv->syscfgex_supported_modes->len);
        for (i = 0; i < self->priv->syscfgex_supported_modes->len; i++) {
            MMHuaweiSyscfgexCombination *huawei_mode;

            huawei_mode = &g_array_index (self->priv->syscfgex_supported_modes,
                                          MMHuaweiSyscfgexCombination,
                                          i);
            mode.allowed = huawei_mode->allowed;
            mode.preferred = huawei_mode->preferred;
            g_array_append_val (combinations, mode);
        }

        self->priv->syscfgex_support = FEATURE_SUPPORTED;

        g_simple_async_result_set_op_res_gpointer (simple,
                                                   combinations,
                                                   (GDestroyNotify)g_array_unref);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    /* If SIM-PIN error, don't mark as feature unsupported; we'll retry later */
    if (error) {
        g_debug ("Error while checking ^SYSCFGEX format: %s", error->message);
        if (g_error_matches (error,
                             MM_MOBILE_EQUIPMENT_ERROR,
                             MM_MOBILE_EQUIPMENT_ERROR_SIM_PIN)) {
            g_simple_async_result_take_error (simple, error);
            g_simple_async_result_complete (simple);
            g_object_unref (simple);
            return;
        }
        g_error_free (error);
    }

    self->priv->syscfgex_support = FEATURE_NOT_SUPPORTED;

    /* Try with SYSCFG */
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "^SYSCFG=?",
                              3,
                              TRUE,
                              (GAsyncReadyCallback)syscfg_test_ready,
                              simple);
}

static void
prefmode_test_ready (MMBroadbandModemHuawei *self,
                     GAsyncResult *res,
                     GSimpleAsyncResult *simple)
{
    const gchar *response;
    GError *error = NULL;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (response)
        self->priv->prefmode_supported_modes = mm_huawei_parse_prefmode_test (response, &error);

    if (self->priv->prefmode_supported_modes) {
        MMModemModeCombination mode;
        guint i;
        GArray *combinations;

        /* Build list of combinations */
        combinations = g_array_sized_new (FALSE,
                                          FALSE,
                                          sizeof (MMModemModeCombination),
                                          self->priv->prefmode_supported_modes->len);
        for (i = 0; i < self->priv->prefmode_supported_modes->len; i++) {
            MMHuaweiPrefmodeCombination *huawei_mode;

            huawei_mode = &g_array_index (self->priv->prefmode_supported_modes,
                                          MMHuaweiPrefmodeCombination,
                                          i);
            mode.allowed = huawei_mode->allowed;
            mode.preferred = huawei_mode->preferred;
            g_array_append_val (combinations, mode);
        }

        self->priv->prefmode_support = FEATURE_SUPPORTED;
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   combinations,
                                                   (GDestroyNotify)g_array_unref);
    } else {
        g_debug ("Error while checking ^PREFMODE format: %s", error->message);
        /* If SIM-PIN error, don't mark as feature unsupported; we'll retry later */
        if (!g_error_matches (error,
                              MM_MOBILE_EQUIPMENT_ERROR,
                              MM_MOBILE_EQUIPMENT_ERROR_SIM_PIN))
            self->priv->prefmode_support = FEATURE_NOT_SUPPORTED;
        g_simple_async_result_take_error (simple, error);
    }

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
load_supported_modes (MMIfaceModem *_self,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_supported_modes);

    if (mm_iface_modem_is_cdma_only (_self)) {
        /* ^PREFMODE only in CDMA-only modems */
        self->priv->syscfg_support = FEATURE_NOT_SUPPORTED;
        self->priv->syscfgex_support = FEATURE_NOT_SUPPORTED;
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "^PREFMODE=?",
                                  3,
                                  TRUE,
                                  (GAsyncReadyCallback)prefmode_test_ready,
                                  result);
        return;
    }

    /* Check SYSCFGEX */
    self->priv->prefmode_support = FEATURE_NOT_SUPPORTED;
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "^SYSCFGEX=?",
                              3,
                              TRUE,
                              (GAsyncReadyCallback)syscfgex_test_ready,
                              result);
}

/*****************************************************************************/
/* Load initial allowed/preferred modes (Modem interface) */

static gboolean
load_current_modes_finish (MMIfaceModem *self,
                           GAsyncResult *res,
                           MMModemMode *allowed,
                           MMModemMode *preferred,
                           GError **error)
{
    MMModemModeCombination *out;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    out = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    *allowed = out->allowed;
    *preferred = out->preferred;
    return TRUE;
}

static void
prefmode_load_current_modes_ready (MMBroadbandModemHuawei *self,
                                   GAsyncResult *res,
                                   GSimpleAsyncResult *simple)
{
    const gchar *response;
    GError *error = NULL;
    const MMHuaweiPrefmodeCombination *current = NULL;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (response)
        current = mm_huawei_parse_prefmode_response (response,
                                                     self->priv->prefmode_supported_modes,
                                                     &error);

    if (error)
        g_simple_async_result_take_error (simple, error);
    else {
        MMModemModeCombination out;

        out.allowed = current->allowed;
        out.preferred = current->preferred;
        g_simple_async_result_set_op_res_gpointer (simple, &out, NULL);
    }
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
syscfg_load_current_modes_ready (MMBroadbandModemHuawei *self,
                                 GAsyncResult *res,
                                 GSimpleAsyncResult *simple)
{
    const gchar *response;
    GError *error = NULL;
    const MMHuaweiSyscfgCombination *current = NULL;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (response)
        current = mm_huawei_parse_syscfg_response (response,
                                                   self->priv->syscfg_supported_modes,
                                                   &error);

    if (error)
        g_simple_async_result_take_error (simple, error);
    else {
        MMModemModeCombination out;

        out.allowed = current->allowed;
        out.preferred = current->preferred;
        g_simple_async_result_set_op_res_gpointer (simple, &out, NULL);
    }

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
syscfgex_load_current_modes_ready (MMBroadbandModemHuawei *self,
                                   GAsyncResult *res,
                                   GSimpleAsyncResult *simple)
{
    const gchar *response;
    GError *error = NULL;
    const MMHuaweiSyscfgexCombination *current = NULL;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (response)
        current = mm_huawei_parse_syscfgex_response (response,
                                                     self->priv->syscfgex_supported_modes,
                                                     &error);
    if (error)
        g_simple_async_result_take_error (simple, error);
    else {
        MMModemModeCombination out;

        out.allowed = current->allowed;
        out.preferred = current->preferred;
        g_simple_async_result_set_op_res_gpointer (simple, &out, NULL);
    }

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
load_current_modes (MMIfaceModem *_self,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);
    GSimpleAsyncResult *result;

    mm_dbg ("loading current modes (huawei)...");

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_current_modes);

    if (self->priv->syscfgex_support == FEATURE_SUPPORTED) {
        g_assert (self->priv->syscfgex_supported_modes != NULL);
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "^SYSCFGEX?",
            3,
            FALSE,
            (GAsyncReadyCallback)syscfgex_load_current_modes_ready,
            result);
        return;
    }

    if (self->priv->syscfg_support == FEATURE_SUPPORTED) {
        g_assert (self->priv->syscfg_supported_modes != NULL);
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "^SYSCFG?",
            3,
            FALSE,
            (GAsyncReadyCallback)syscfg_load_current_modes_ready,
            result);
        return;
    }

    if (self->priv->prefmode_support == FEATURE_SUPPORTED) {
        g_assert (self->priv->prefmode_supported_modes != NULL);
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "^PREFMODE?",
            3,
            FALSE,
            (GAsyncReadyCallback)prefmode_load_current_modes_ready,
            result);
        return;
    }

    g_simple_async_result_set_error (result,
                                     MM_CORE_ERROR,
                                     MM_CORE_ERROR_FAILED,
                                     "Unable to load current modes");
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Set current modes (Modem interface) */

static gboolean
set_current_modes_finish (MMIfaceModem *self,
                          GAsyncResult *res,
                          GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
set_current_modes_ready (MMBroadbandModemHuawei *self,
                         GAsyncResult *res,
                         GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (error)
        /* Let the error be critical. */
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gboolean (simple, TRUE);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static gboolean
prefmode_set_current_modes (MMBroadbandModemHuawei *self,
                            MMModemMode allowed,
                            MMModemMode preferred,
                            GSimpleAsyncResult *simple,
                            GError **error)
{
    guint i;
    MMHuaweiPrefmodeCombination *found = NULL;
    gchar *command;

    for (i = 0; i < self->priv->prefmode_supported_modes->len; i++) {
        MMHuaweiPrefmodeCombination *single;

        single = &g_array_index (self->priv->prefmode_supported_modes,
                                 MMHuaweiPrefmodeCombination,
                                 i);
        if (single->allowed == allowed && single->preferred == preferred) {
            found = single;
            break;
        }
    }

    if (!found) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_NOT_FOUND,
                     "Requested mode ^PREFMODE combination not found");
        return FALSE;
    }

    command = g_strdup_printf ("^PREFMODE=%u", found->prefmode);
    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        command,
        3,
        FALSE,
        (GAsyncReadyCallback)set_current_modes_ready,
        simple);
    g_free (command);
    return TRUE;
}

static gboolean
syscfg_set_current_modes (MMBroadbandModemHuawei *self,
                          MMModemMode allowed,
                          MMModemMode preferred,
                          GSimpleAsyncResult *simple,
                          GError **error)
{
    guint i;
    MMHuaweiSyscfgCombination *found = NULL;
    gchar *command;

    for (i = 0; i < self->priv->syscfg_supported_modes->len; i++) {
        MMHuaweiSyscfgCombination *single;

        single = &g_array_index (self->priv->syscfg_supported_modes,
                                 MMHuaweiSyscfgCombination,
                                 i);
        if (single->allowed == allowed && single->preferred == preferred) {
            found = single;
            break;
        }
    }

    if (!found) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_NOT_FOUND,
                     "Requested mode ^SYSCFG combination not found");
        return FALSE;
    }

    command = g_strdup_printf ("^SYSCFG=%u,%u,40000000,2,4",
                               found->mode,
                               found->acqorder);
    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        command,
        3,
        FALSE,
        (GAsyncReadyCallback)set_current_modes_ready,
        simple);
    g_free (command);
    return TRUE;
}

static gboolean
syscfgex_set_current_modes (MMBroadbandModemHuawei *self,
                            MMModemMode allowed,
                            MMModemMode preferred,
                            GSimpleAsyncResult *simple,
                            GError **error)
{
    guint i;
    MMHuaweiSyscfgexCombination *found = NULL;
    gchar *command;

    for (i = 0; i < self->priv->syscfgex_supported_modes->len; i++) {
        MMHuaweiSyscfgexCombination *single;

        single = &g_array_index (self->priv->syscfgex_supported_modes,
                                 MMHuaweiSyscfgexCombination,
                                 i);
        if (single->allowed == allowed && single->preferred == preferred) {
            found = single;
            break;
        }
    }

    if (!found) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_NOT_FOUND,
                     "Requested mode ^SYSCFGEX combination not found");
        return FALSE;
    }

    command = g_strdup_printf ("^SYSCFGEX=\"%s\",3fffffff,2,4,7fffffffffffffff,,",
                               found->mode_str);
    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        command,
        3,
        FALSE,
        (GAsyncReadyCallback)set_current_modes_ready,
        simple);
    g_free (command);
    return TRUE;
}

static void
set_current_modes (MMIfaceModem *_self,
                   MMModemMode allowed,
                   MMModemMode preferred,
                   GAsyncReadyCallback callback,
                   gpointer user_data)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);
    GSimpleAsyncResult *result;
    GError *error = NULL;

    mm_dbg ("setting current modes (huawei)...");

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        set_current_modes);

    if (self->priv->syscfgex_support == FEATURE_SUPPORTED)
        syscfgex_set_current_modes (self, allowed, preferred, result, &error);
    else if (self->priv->syscfg_support == FEATURE_SUPPORTED)
        syscfg_set_current_modes (self, allowed, preferred, result, &error);
    else if (self->priv->prefmode_support == FEATURE_SUPPORTED)
        prefmode_set_current_modes (self, allowed, preferred, result, &error);
    else
        error = g_error_new (MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Setting current modes is not supported");

    if (error) {
        g_simple_async_result_take_error (result, error);
        g_simple_async_result_complete_in_idle (result);
        g_object_unref (result);
    }
}

/*****************************************************************************/
/* Setup/Cleanup unsolicited events (3GPP interface) */

static void
huawei_signal_changed (MMPortSerialAt *port,
                       GMatchInfo *match_info,
                       MMBroadbandModemHuawei *self)
{
    guint quality = 0;

    if (!mm_get_uint_from_match_info (match_info, 1, &quality))
        return;

    if (quality == 99) {
        /* 99 means unknown */
        quality = 0;
    } else {
        /* Normalize the quality */
        quality = CLAMP (quality, 0, 31) * 100 / 31;
    }

    mm_dbg ("3GPP signal quality: %u", quality);
    mm_iface_modem_update_signal_quality (MM_IFACE_MODEM (self), (guint)quality);
}

static void
huawei_mode_changed (MMPortSerialAt *port,
                     GMatchInfo *match_info,
                     MMBroadbandModemHuawei *self)
{
    MMModemAccessTechnology act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    gchar *str;
    gint a;
    guint32 mask = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;

    str = g_match_info_fetch (match_info, 1);
    a = atoi (str);
    g_free (str);

    /* CDMA/EVDO devices may not send this */
    str = g_match_info_fetch (match_info, 2);
    if (str[0])
        act = huawei_sysinfo_submode_to_act (atoi (str));
    g_free (str);

    switch (a) {
    case 3:
        /* GSM/GPRS mode */
        if (act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN &&
            (act < MM_MODEM_ACCESS_TECHNOLOGY_GSM ||
             act > MM_MODEM_ACCESS_TECHNOLOGY_EDGE)) {
            str = mm_modem_access_technology_build_string_from_mask (act);
            mm_warn ("Unexpected access technology (%s) in GSM/GPRS mode", str);
            g_free (str);
            act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
        }
        mask = MM_IFACE_MODEM_3GPP_ALL_ACCESS_TECHNOLOGIES_MASK;
        break;

    case 5:
        /* WCDMA mode */
        if (act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN &&
            (act < MM_MODEM_ACCESS_TECHNOLOGY_UMTS ||
             act > MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS)) {
            str = mm_modem_access_technology_build_string_from_mask (act);
            mm_warn ("Unexpected access technology (%s) in WCDMA mode", str);
            g_free (str);
            act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
        }
        mask = MM_IFACE_MODEM_3GPP_ALL_ACCESS_TECHNOLOGIES_MASK;
        break;

    case 2:
        /* CDMA mode */
        if (act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN &&
            act != MM_MODEM_ACCESS_TECHNOLOGY_1XRTT) {
            str = mm_modem_access_technology_build_string_from_mask (act);
            mm_warn ("Unexpected access technology (%s) in CDMA mode", str);
            g_free (str);
            act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
        }
        if (act == MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN)
            act = MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
        mask = MM_IFACE_MODEM_CDMA_ALL_ACCESS_TECHNOLOGIES_MASK;
        break;

    case 4:  /* HDR mode */
    case 8:  /* CDMA/HDR hybrid mode */
        if (act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN &&
            (act < MM_MODEM_ACCESS_TECHNOLOGY_EVDO0 ||
             act > MM_MODEM_ACCESS_TECHNOLOGY_EVDOB)) {
            str = mm_modem_access_technology_build_string_from_mask (act);
            mm_warn ("Unexpected access technology (%s) in EVDO mode", str);
            g_free (str);
            act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
        }
        if (act == MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN)
            act = MM_MODEM_ACCESS_TECHNOLOGY_EVDO0;
        mask = MM_IFACE_MODEM_CDMA_ALL_ACCESS_TECHNOLOGIES_MASK;
        break;

    case 0:
        act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
        break;

    default:
        mm_warn ("Unexpected mode change value reported: '%d'", a);
        return;
    }

    str = mm_modem_access_technology_build_string_from_mask (act);
    mm_dbg ("Access Technology: '%s'", str);
    g_free (str);

    mm_iface_modem_update_access_technologies (MM_IFACE_MODEM (self), act, mask);
}

static void
huawei_status_changed (MMPortSerialAt *port,
                       GMatchInfo *match_info,
                       MMBroadbandModemHuawei *self)
{
    gchar *str;
    gint n1, n2, n3, n4, n5, n6, n7;

    str = g_match_info_fetch (match_info, 1);
    if (sscanf (str, "%x,%x,%x,%x,%x,%x,%x", &n1, &n2, &n3, &n4, &n5, &n6, &n7)) {
        mm_dbg ("Duration: %d Up: %d Kbps Down: %d Kbps Total: %d Total: %d\n",
                n1, n2 * 8 / 1000, n3  * 8 / 1000, n4 / 1024, n5 / 1024);
    }
    g_free (str);
}

typedef struct {
    gboolean ipv4_available;
    gboolean ipv4_connected;
    gboolean ipv6_available;
    gboolean ipv6_connected;
} NdisstatResult;

static void
bearer_report_connection_status (MMBaseBearer *bearer,
                                 NdisstatResult *ndisstat_result)
{
    if (ndisstat_result->ipv4_available) {
        /* TODO: MMBroadbandBearerHuawei does not currently support IPv6.
         * When it does, we should check the IP family associated with each bearer.
         *
         * Also, send DISCONNECTING so that we give some time before actually
         * disconnecting the connection */
        mm_base_bearer_report_connection_status (bearer,
                                                 ndisstat_result->ipv4_connected ?
                                                 MM_BEARER_CONNECTION_STATUS_CONNECTED :
                                                 MM_BEARER_CONNECTION_STATUS_DISCONNECTING);
    }
}

static void
huawei_ndisstat_changed (MMPortSerialAt *port,
                         GMatchInfo *match_info,
                         MMBroadbandModemHuawei *self)
{
    gchar *str;
    NdisstatResult ndisstat_result;
    GError *error = NULL;
    MMBearerList *list = NULL;

    str = g_match_info_fetch (match_info, 1);
    if (!mm_huawei_parse_ndisstatqry_response (str,
                                               &ndisstat_result.ipv4_available,
                                               &ndisstat_result.ipv4_connected,
                                               &ndisstat_result.ipv6_available,
                                               &ndisstat_result.ipv6_connected,
                                               &error)) {
        mm_dbg ("Ignore invalid ^NDISSTAT unsolicited message: '%s' (error %s)",
                str, error->message);
        g_error_free (error);
        g_free (str);
        return;
    }
    g_free (str);

    mm_dbg ("NDIS status: IPv4 %s, IPv6 %s",
            ndisstat_result.ipv4_available ?
            (ndisstat_result.ipv4_connected ? "connected" : "disconnected") : "not available",
            ndisstat_result.ipv6_available ?
            (ndisstat_result.ipv6_connected ? "connected" : "disconnected") : "not available");

    /* If empty bearer list, nothing else to do */
    g_object_get (self,
                  MM_IFACE_MODEM_BEARER_LIST, &list,
                  NULL);
    if (!list)
        return;

    mm_bearer_list_foreach (list,
                            (MMBearerListForeachFunc)bearer_report_connection_status,
                            &ndisstat_result);

    g_object_unref (list);
}

static void
set_3gpp_unsolicited_events_handlers (MMBroadbandModemHuawei *self,
                                      gboolean enable)
{
    GList *ports, *l;

    ports = get_at_port_list (self);

    /* Enable/disable unsolicited events in given port */
    for (l = ports; l; l = g_list_next (l)) {
        MMPortSerialAt *port = MM_PORT_SERIAL_AT (l->data);

        /* Signal quality related */
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->rssi_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)huawei_signal_changed : NULL,
            enable ? self : NULL,
            NULL);

        /* Access technology related */
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->mode_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)huawei_mode_changed : NULL,
            enable ? self : NULL,
            NULL);

        /* Connection status related */
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->dsflowrpt_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)huawei_status_changed : NULL,
            enable ? self : NULL,
            NULL);

        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->ndisstat_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)huawei_ndisstat_changed : NULL,
            enable ? self : NULL,
            NULL);
    }

    g_list_free_full (ports, (GDestroyNotify)g_object_unref);
}

static gboolean
modem_3gpp_setup_cleanup_unsolicited_events_finish (MMIfaceModem3gpp *self,
                                                    GAsyncResult *res,
                                                    GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
parent_3gpp_setup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                            GAsyncResult *res,
                                            GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->setup_unsolicited_events_finish (self, res, &error))
        g_simple_async_result_take_error (simple, error);
    else {
        /* Our own setup now */
        set_3gpp_unsolicited_events_handlers (MM_BROADBAND_MODEM_HUAWEI (self), TRUE);
        g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res), TRUE);
    }

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_3gpp_setup_unsolicited_events (MMIfaceModem3gpp *self,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_setup_unsolicited_events);

    /* Chain up parent's setup */
    iface_modem_3gpp_parent->setup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_3gpp_setup_unsolicited_events_ready,
        result);
}

static void
parent_3gpp_cleanup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                              GAsyncResult *res,
                                              GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->cleanup_unsolicited_events_finish (self, res, &error))
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res), TRUE);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_3gpp_cleanup_unsolicited_events (MMIfaceModem3gpp *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_cleanup_unsolicited_events);

    /* Our own cleanup first */
    set_3gpp_unsolicited_events_handlers (MM_BROADBAND_MODEM_HUAWEI (self), FALSE);

    /* And now chain up parent's cleanup */
    iface_modem_3gpp_parent->cleanup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_3gpp_cleanup_unsolicited_events_ready,
        result);
}

/*****************************************************************************/
/* Enabling unsolicited events (3GPP interface) */

static gboolean
modem_3gpp_enable_unsolicited_events_finish (MMIfaceModem3gpp *self,
                                             GAsyncResult *res,
                                             GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
own_enable_unsolicited_events_ready (MMBaseModem *self,
                                     GAsyncResult *res,
                                     GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    mm_base_modem_at_sequence_full_finish (self, res, NULL, &error);
    if (error)
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gboolean (simple, TRUE);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static const MMBaseModemAtCommand unsolicited_enable_sequence[] = {
    /* With ^PORTSEL we specify whether we want the PCUI port (0) or the
     * modem port (1) to receive the unsolicited messages */
    { "^PORTSEL=0", 5, FALSE, NULL },
    { "^CURC=1",    3, FALSE, NULL },
    { NULL }
};

static void
parent_enable_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                        GAsyncResult *res,
                                        GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->enable_unsolicited_events_finish (self, res, &error)) {
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
    }

    /* Our own enable now */
    mm_base_modem_at_sequence_full (
        MM_BASE_MODEM (self),
        mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
        unsolicited_enable_sequence,
        NULL, /* response_processor_context */
        NULL, /* response_processor_context_free */
        NULL, /* cancellable */
        (GAsyncReadyCallback)own_enable_unsolicited_events_ready,
        simple);
}

static void
modem_3gpp_enable_unsolicited_events (MMIfaceModem3gpp *self,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_enable_unsolicited_events);

    /* Chain up parent's enable */
    iface_modem_3gpp_parent->enable_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_enable_unsolicited_events_ready,
        result);
}

/*****************************************************************************/
/* Disabling unsolicited events (3GPP interface) */

static gboolean
modem_3gpp_disable_unsolicited_events_finish (MMIfaceModem3gpp *self,
                                              GAsyncResult *res,
                                              GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
parent_disable_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                         GAsyncResult *res,
                                         GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->disable_unsolicited_events_finish (self, res, &error))
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gboolean (simple, TRUE);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
own_disable_unsolicited_events_ready (MMBaseModem *self,
                                      GAsyncResult *res,
                                      GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    mm_base_modem_at_command_full_finish (self, res, &error);
    if (error) {
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    /* Next, chain up parent's disable */
    iface_modem_3gpp_parent->disable_unsolicited_events (
        MM_IFACE_MODEM_3GPP (self),
        (GAsyncReadyCallback)parent_disable_unsolicited_events_ready,
        simple);
}

static void
modem_3gpp_disable_unsolicited_events (MMIfaceModem3gpp *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_disable_unsolicited_events);

    /* Our own disable first */
    mm_base_modem_at_command_full (
        MM_BASE_MODEM (self),
        mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
        "^CURC=0",
        5,
        FALSE, /* allow_cached */
        FALSE, /* raw */
        NULL, /* cancellable */
        (GAsyncReadyCallback)own_disable_unsolicited_events_ready,
        result);
}

/*****************************************************************************/
/* Operator Name loading (3GPP interface) */

static gchar *
modem_3gpp_load_operator_name_finish (MMIfaceModem3gpp *self,
                                      GAsyncResult *res,
                                      GError **error)
{
    const gchar *result;
    gchar *operator_name;

    result = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!result)
        return NULL;

    /* Despite +CSCS? may claim supporting UCS2, Huawei modems always report the
     * operator name in ASCII in a +COPS response. Thus, we ignore the current
     * charset claimed by the modem and assume the charset is IRA when parsing
     * the operator name.
     */
    operator_name = mm_3gpp_parse_operator (result, MM_MODEM_CHARSET_IRA);
    if (operator_name)
        mm_dbg ("loaded Operator Name: %s", operator_name);

    return operator_name;
}

static void
modem_3gpp_load_operator_name (MMIfaceModem3gpp *self,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
    mm_dbg ("loading Operator Name (huawei)...");
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+COPS=3,0;+COPS?",
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* Create Bearer (Modem interface) */

typedef struct {
    MMBroadbandModemHuawei *self;
    GSimpleAsyncResult *result;
    MMBearerProperties *properties;
} CreateBearerContext;

static void
create_bearer_context_complete_and_free (CreateBearerContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->self);
    g_object_unref (ctx->properties);
    g_slice_free (CreateBearerContext, ctx);
}

static MMBaseBearer *
huawei_modem_create_bearer_finish (MMIfaceModem *self,
                                   GAsyncResult *res,
                                   GError **error)
{
    MMBaseBearer *bearer;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    bearer = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    mm_dbg ("New huawei bearer created at DBus path '%s'", mm_base_bearer_get_path (bearer));
    return g_object_ref (bearer);
}

static void
broadband_bearer_huawei_new_ready (GObject *source,
                                   GAsyncResult *res,
                                   CreateBearerContext *ctx)
{
    MMBaseBearer *bearer;
    GError *error = NULL;

    bearer = mm_broadband_bearer_huawei_new_finish (res, &error);
    if (!bearer)
        g_simple_async_result_take_error (ctx->result, error);
    else
        g_simple_async_result_set_op_res_gpointer (ctx->result, bearer, (GDestroyNotify)g_object_unref);
    create_bearer_context_complete_and_free (ctx);
}

static void
broadband_bearer_new_ready (GObject *source,
                            GAsyncResult *res,
                            CreateBearerContext *ctx)
{
    MMBaseBearer *bearer;
    GError *error = NULL;

    bearer = mm_broadband_bearer_new_finish (res, &error);
    if (!bearer)
        g_simple_async_result_take_error (ctx->result, error);
    else
        g_simple_async_result_set_op_res_gpointer (ctx->result, bearer, (GDestroyNotify)g_object_unref);
    create_bearer_context_complete_and_free (ctx);
}

static void
create_bearer_for_net_port (CreateBearerContext *ctx)
{
    switch (ctx->self->priv->ndisdup_support) {
    case FEATURE_SUPPORT_UNKNOWN:
        g_assert_not_reached ();
    case FEATURE_NOT_SUPPORTED:
        mm_dbg ("^NDISDUP not supported, creating default bearer...");
        mm_broadband_bearer_new (MM_BROADBAND_MODEM (ctx->self),
                                 ctx->properties,
                                 NULL, /* cancellable */
                                 (GAsyncReadyCallback)broadband_bearer_new_ready,
                                 ctx);
        return;
    case FEATURE_SUPPORTED:
        mm_dbg ("^NDISDUP supported, creating huawei bearer...");
        mm_broadband_bearer_huawei_new (MM_BROADBAND_MODEM_HUAWEI (ctx->self),
                                        ctx->properties,
                                        NULL, /* cancellable */
                                        (GAsyncReadyCallback)broadband_bearer_huawei_new_ready,
                                        ctx);
        return;
    }
}

static MMPortSerialAt *
peek_port_at_for_data (MMBroadbandModemHuawei *self,
                       MMPort *port)
{
    GList *cdc_wdm_at_ports, *l;
    const gchar *net_port_parent_path;

    g_warn_if_fail (mm_port_get_subsys (port) == MM_PORT_SUBSYS_NET);
    net_port_parent_path = mm_port_get_parent_path (port);
    if (!net_port_parent_path) {
        g_warning ("(%s) no parent path for net port", mm_port_get_device (port));
        return NULL;
    }

    /* Find the CDC-WDM port on the same USB interface as the given net port */
    cdc_wdm_at_ports = mm_base_modem_find_ports (MM_BASE_MODEM (self),
                                                 MM_PORT_SUBSYS_USB,
                                                 MM_PORT_TYPE_AT,
                                                 NULL);
    for (l = cdc_wdm_at_ports; l; l = g_list_next (l)) {
        const gchar  *wdm_port_parent_path;

        g_assert (MM_IS_PORT_SERIAL_AT (l->data));
        wdm_port_parent_path = mm_port_get_parent_path (MM_PORT (l->data));
        if (wdm_port_parent_path && g_str_equal (wdm_port_parent_path, net_port_parent_path))
            return MM_PORT_SERIAL_AT (l->data);
    }

    return NULL;
}


MMPortSerialAt *
mm_broadband_modem_huawei_peek_port_at_for_data (MMBroadbandModemHuawei *self,
                                                 MMPort *port)
{
    MMPortSerialAt *found;

    g_assert (self->priv->ndisdup_support == FEATURE_SUPPORTED);

    found = peek_port_at_for_data (self, port);
    if (!found)
        mm_warn ("Couldn't find associated cdc-wdm port for 'net/%s'",
                 mm_port_get_device (port));
    return found;
}

static void
ensure_ndisdup_support_checked (MMBroadbandModemHuawei *self,
                                MMPort *port)
{
    GUdevClient *client;
    GUdevDevice *data_device;

    /* Check NDISDUP support the first time we need it */
    if (self->priv->ndisdup_support != FEATURE_SUPPORT_UNKNOWN)
        return;

    /* First, check for devices which support NDISDUP on any AT port. These
     * devices are tagged by udev */
    client = g_udev_client_new (NULL);
    data_device = (g_udev_client_query_by_subsystem_and_name (
                       client,
                       "net",
                       mm_port_get_device (port)));
    if (data_device && g_udev_device_get_property_as_boolean (data_device, "ID_MM_HUAWEI_NDISDUP_SUPPORTED")) {
        mm_dbg ("This device (%s) can support ndisdup feature", mm_port_get_device (port));
        self->priv->ndisdup_support = FEATURE_SUPPORTED;
        goto out;
    }

    /* Then, look for devices which have both a net port and a cdc-wdm
     * AT-capable port. We assume that these devices allow NDISDUP only
     * when issued in the cdc-wdm port. */
    if (peek_port_at_for_data (self, port)) {
        mm_dbg ("This device (%s) can support ndisdup feature on non-serial AT port",
                mm_port_get_device (port));
        self->priv->ndisdup_support = FEATURE_SUPPORTED;
        goto out;
    }

    mm_dbg ("This device (%s) can not support ndisdup feature", mm_port_get_device (port));
    self->priv->ndisdup_support = FEATURE_NOT_SUPPORTED;

out:
    if (data_device)
        g_object_unref (data_device);
    if (client)
        g_object_unref (client);
}

static void
huawei_modem_create_bearer (MMIfaceModem *self,
                            MMBearerProperties *properties,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    CreateBearerContext *ctx;
    MMPort *port;

    ctx = g_slice_new0 (CreateBearerContext);
    ctx->self = g_object_ref (self);
    ctx->properties = g_object_ref (properties);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             huawei_modem_create_bearer);

    port = mm_base_modem_peek_best_data_port (MM_BASE_MODEM (self), MM_PORT_TYPE_NET);
    if (port) {
        ensure_ndisdup_support_checked (ctx->self, port);
        create_bearer_for_net_port (ctx);
        return;
    }

    mm_dbg ("Creating default bearer...");
    mm_broadband_bearer_new (MM_BROADBAND_MODEM (self),
                             properties,
                             NULL, /* cancellable */
                             (GAsyncReadyCallback)broadband_bearer_new_ready,
                             ctx);
}

/*****************************************************************************/
/* USSD encode/decode (3GPP-USSD interface) */

static gchar *
encode (MMIfaceModem3gppUssd *self,
        const gchar *command,
        guint *scheme,
        GError **error)
{
    gchar *hex;
    guint8 *gsm, *packed;
    guint32 len = 0, packed_len = 0;

    *scheme = MM_MODEM_GSM_USSD_SCHEME_7BIT;
    gsm = mm_charset_utf8_to_unpacked_gsm (command, &len);

    /* If command is a multiple of 7 characters long, Huawei firmwares
     * apparently want that padded.  Maybe all modems?
     */
    if (len % 7 == 0) {
        gsm = g_realloc (gsm, len + 1);
        gsm[len] = 0x0d;
        len++;
    }

    packed = gsm_pack (gsm, len, 0, &packed_len);
    hex = mm_utils_bin2hexstr (packed, packed_len);
    g_free (packed);
    g_free (gsm);

    return hex;
}

static gchar *
decode (MMIfaceModem3gppUssd *self,
        const gchar *reply,
        GError **error)
{
    gchar *bin, *utf8;
    guint8 *unpacked;
    gsize bin_len;
    guint32 unpacked_len;

    bin = mm_utils_hexstr2bin (reply, &bin_len);
    unpacked = gsm_unpack ((guint8*) bin, (bin_len * 8) / 7, 0, &unpacked_len);
    /* if the last character in a 7-byte block is padding, then drop it */
    if ((bin_len % 7 == 0) && (unpacked[unpacked_len - 1] == 0x0d))
        unpacked_len--;
    utf8 = (char*) mm_charset_gsm_unpacked_to_utf8 (unpacked, unpacked_len);

    g_free (bin);
    g_free (unpacked);
    return utf8;
}

/*****************************************************************************/

static void
huawei_1x_signal_changed (MMPortSerialAt *port,
                          GMatchInfo *match_info,
                          MMBroadbandModemHuawei *self)
{
    guint quality = 0;

    if (!mm_get_uint_from_match_info (match_info, 1, &quality))
        return;

    quality = CLAMP (quality, 0, 100);
    mm_dbg ("1X signal quality: %u", quality);
    mm_iface_modem_update_signal_quality (MM_IFACE_MODEM (self), (guint)quality);
}

static void
huawei_evdo_signal_changed (MMPortSerialAt *port,
                            GMatchInfo *match_info,
                            MMBroadbandModemHuawei *self)
{
    guint quality = 0;

    if (!mm_get_uint_from_match_info (match_info, 1, &quality))
        return;

    quality = CLAMP (quality, 0, 100);
    mm_dbg ("EVDO signal quality: %u", quality);
    mm_iface_modem_update_signal_quality (MM_IFACE_MODEM (self), (guint)quality);
}

/* Signal quality loading (Modem interface) */

static guint
modem_load_signal_quality_finish (MMIfaceModem *self,
                                  GAsyncResult *res,
                                  GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return 0;

    return GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (
                                 G_SIMPLE_ASYNC_RESULT (res)));
}

static void
parent_load_signal_quality_ready (MMIfaceModem *self,
                                  GAsyncResult *res,
                                  GSimpleAsyncResult *simple)
{
    GError *error = NULL;
    guint signal_quality;

    signal_quality = iface_modem_parent->load_signal_quality_finish (self, res, &error);
    if (error)
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   GUINT_TO_POINTER (signal_quality),
                                                   NULL);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
signal_ready (MMBaseModem *self,
              GAsyncResult *res,
              GSimpleAsyncResult *simple)
{
    const gchar *response, *command;
    gchar buf[5];
    guint quality = 0, i = 0;

    response = mm_base_modem_at_command_finish (self, res, NULL);
    if (!response) {
        /* Fallback to parent's method */
        iface_modem_parent->load_signal_quality (
            MM_IFACE_MODEM (self),
            (GAsyncReadyCallback)parent_load_signal_quality_ready,
            simple);
        return;
    }

    command = g_object_get_data (G_OBJECT (simple), "command");
    g_assert (command);
    response = mm_strip_tag (response, command);
    /* 'command' won't include the trailing ':' in the response, so strip that */
    while ((*response == ':') || isspace (*response))
        response++;

    /* Sanitize response for mm_get_uint_from_str() which wants only digits */
    memset (buf, 0, sizeof (buf));
    while (i < (sizeof (buf) - 1) && isdigit (*response))
        buf[i++] = *response++;

    if (mm_get_uint_from_str (buf, &quality)) {
        quality = CLAMP (quality, 0, 100);
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   GUINT_TO_POINTER (quality),
                                                   NULL);
    } else {
        g_simple_async_result_set_error (simple,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Couldn't parse %s response: '%s'",
                                         command, response);
    }

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_load_signal_quality (MMIfaceModem *self,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    GSimpleAsyncResult *result;
    MMModemCdmaRegistrationState evdo_state = MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN;
    const char *command = "^CSQLVL";

    mm_dbg ("loading signal quality...");
    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_signal_quality);

    /* 3GPP modems can just run parent's signal quality loading */
    if (mm_iface_modem_is_3gpp (self)) {
        iface_modem_parent->load_signal_quality (
            self,
            (GAsyncReadyCallback)parent_load_signal_quality_ready,
            result);
        return;
    }

    /* CDMA modems need custom signal quality loading */

    g_object_get (G_OBJECT (self),
                  MM_IFACE_MODEM_CDMA_EVDO_REGISTRATION_STATE, &evdo_state,
                  NULL);
    if (evdo_state > MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN)
        command = "^HDRCSQLVL";
    g_object_set_data (G_OBJECT (result), "command", (gpointer) command);

    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        command,
        3,
        FALSE,
        (GAsyncReadyCallback)signal_ready,
        result);
}

/*****************************************************************************/
/* Setup/Cleanup unsolicited events (CDMA interface) */

static void
set_cdma_unsolicited_events_handlers (MMBroadbandModemHuawei *self,
                                      gboolean enable)
{
    GList *ports, *l;

    ports = get_at_port_list (self);

    /* Enable/disable unsolicited events in given port */
    for (l = ports; l; l = g_list_next (l)) {
        MMPortSerialAt *port = MM_PORT_SERIAL_AT (l->data);

        /* Signal quality related */
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->rssilvl_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)huawei_1x_signal_changed : NULL,
            enable ? self : NULL,
            NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->hrssilvl_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)huawei_evdo_signal_changed : NULL,
            enable ? self : NULL,
            NULL);
        /* Access technology related */
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->mode_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)huawei_mode_changed : NULL,
            enable ? self : NULL,
            NULL);
    }

    g_list_free_full (ports, (GDestroyNotify)g_object_unref);
}

static gboolean
modem_cdma_setup_cleanup_unsolicited_events_finish (MMIfaceModemCdma *self,
                                                    GAsyncResult *res,
                                                    GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
parent_cdma_setup_unsolicited_events_ready (MMIfaceModemCdma *self,
                                            GAsyncResult *res,
                                            GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    if (!iface_modem_cdma_parent->setup_unsolicited_events_finish (self, res, &error))
        g_simple_async_result_take_error (simple, error);
    else {
        /* Our own setup now */
        set_cdma_unsolicited_events_handlers (MM_BROADBAND_MODEM_HUAWEI (self), TRUE);
        g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res), TRUE);
    }

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_cdma_setup_unsolicited_events (MMIfaceModemCdma *self,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_cdma_setup_unsolicited_events);

    /* Chain up parent's setup if needed */
    if (iface_modem_cdma_parent->setup_unsolicited_events &&
        iface_modem_cdma_parent->setup_unsolicited_events_finish) {
        iface_modem_cdma_parent->setup_unsolicited_events (
            self,
            (GAsyncReadyCallback)parent_cdma_setup_unsolicited_events_ready,
            result);
        return;
    }

    /* Otherwise just run our setup and complete */
    set_cdma_unsolicited_events_handlers (MM_BROADBAND_MODEM_HUAWEI (self), TRUE);
    g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (result), TRUE);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

static void
parent_cdma_cleanup_unsolicited_events_ready (MMIfaceModemCdma *self,
                                              GAsyncResult *res,
                                              GSimpleAsyncResult *simple)
{
    GError *error = NULL;

    if (!iface_modem_cdma_parent->cleanup_unsolicited_events_finish (self, res, &error))
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res), TRUE);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_cdma_cleanup_unsolicited_events (MMIfaceModemCdma *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_cdma_cleanup_unsolicited_events);

    /* Our own cleanup first */
    set_cdma_unsolicited_events_handlers (MM_BROADBAND_MODEM_HUAWEI (self), FALSE);

    /* Chain up parent's setup if needed */
    if (iface_modem_cdma_parent->cleanup_unsolicited_events &&
        iface_modem_cdma_parent->cleanup_unsolicited_events_finish) {
        iface_modem_cdma_parent->cleanup_unsolicited_events (
            self,
            (GAsyncReadyCallback)parent_cdma_cleanup_unsolicited_events_ready,
            result);
        return;
    }

    /* Otherwise we're done */
    g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (result), TRUE);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Setup registration checks (CDMA interface) */

typedef struct {
    gboolean skip_qcdm_call_manager_step;
    gboolean skip_qcdm_hdr_step;
    gboolean skip_at_cdma_service_status_step;
    gboolean skip_at_cdma1x_serving_system_step;
    gboolean skip_detailed_registration_state;
} SetupRegistrationChecksResults;

static gboolean
setup_registration_checks_finish (MMIfaceModemCdma *self,
                                  GAsyncResult *res,
                                  gboolean *skip_qcdm_call_manager_step,
                                  gboolean *skip_qcdm_hdr_step,
                                  gboolean *skip_at_cdma_service_status_step,
                                  gboolean *skip_at_cdma1x_serving_system_step,
                                  gboolean *skip_detailed_registration_state,
                                  GError **error)
{
    SetupRegistrationChecksResults *results;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    results = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    *skip_qcdm_call_manager_step = results->skip_qcdm_call_manager_step;
    *skip_qcdm_hdr_step = results->skip_qcdm_hdr_step;
    *skip_at_cdma_service_status_step = results->skip_at_cdma_service_status_step;
    *skip_at_cdma1x_serving_system_step = results->skip_at_cdma1x_serving_system_step;
    *skip_detailed_registration_state = results->skip_detailed_registration_state;
    return TRUE;
}

static void
parent_setup_registration_checks_ready (MMIfaceModemCdma *self,
                                        GAsyncResult *res,
                                        GSimpleAsyncResult *simple)
{
    GError *error = NULL;
    SetupRegistrationChecksResults results = { 0 };

    if (!iface_modem_cdma_parent->setup_registration_checks_finish (self,
                                                                    res,
                                                                    &results.skip_qcdm_call_manager_step,
                                                                    &results.skip_qcdm_hdr_step,
                                                                    &results.skip_at_cdma_service_status_step,
                                                                    &results.skip_at_cdma1x_serving_system_step,
                                                                    &results.skip_detailed_registration_state,
                                                                    &error)) {
        g_simple_async_result_take_error (simple, error);
    } else {
        gboolean evdo_supported = FALSE;

        g_object_get (self,
                      MM_IFACE_MODEM_CDMA_EVDO_NETWORK_SUPPORTED, &evdo_supported,
                      NULL);

        /* Don't use AT+CSS on EVDO-capable hardware for determining registration
         * status, because often the device will have only an EVDO connection and
         * AT+CSS won't necessarily report EVDO registration status, only 1X.
         */
        if (evdo_supported)
            results.skip_at_cdma1x_serving_system_step = TRUE;

        /* Force to always use the detailed registration checks, as we have
         * ^SYSINFO for that */
        results.skip_detailed_registration_state = FALSE;

        g_simple_async_result_set_op_res_gpointer (simple, &results, NULL);
    }

    /* All done. NOTE: complete NOT in idle! */
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
setup_registration_checks (MMIfaceModemCdma *self,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        setup_registration_checks);

    /* Run parent's checks first */
    iface_modem_cdma_parent->setup_registration_checks (self,
                                                        (GAsyncReadyCallback)parent_setup_registration_checks_ready,
                                                        result);
}

/*****************************************************************************/
/* Detailed registration state (CDMA interface) */

typedef struct {
    MMModemCdmaRegistrationState detailed_cdma1x_state;
    MMModemCdmaRegistrationState detailed_evdo_state;
} DetailedRegistrationStateResults;

typedef struct {
    MMBroadbandModem *self;
    GSimpleAsyncResult *result;
    DetailedRegistrationStateResults state;
} DetailedRegistrationStateContext;

static void
detailed_registration_state_context_complete_and_free (DetailedRegistrationStateContext *ctx)
{
    /* Always not in idle! we're passing a struct in stack as result */
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static gboolean
get_detailed_registration_state_finish (MMIfaceModemCdma *self,
                                        GAsyncResult *res,
                                        MMModemCdmaRegistrationState *detailed_cdma1x_state,
                                        MMModemCdmaRegistrationState *detailed_evdo_state,
                                        GError **error)
{
    DetailedRegistrationStateResults *results;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    results = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    *detailed_cdma1x_state = results->detailed_cdma1x_state;
    *detailed_evdo_state = results->detailed_evdo_state;
    return TRUE;
}

static void
registration_state_sysinfo_ready (MMBroadbandModemHuawei *self,
                                  GAsyncResult *res,
                                  DetailedRegistrationStateContext *ctx)
{
    gboolean extended = FALSE;
    guint srv_status = 0;
    guint sys_mode = 0;
    guint roam_status = 0;

    if (!sysinfo_finish (self,
                         res,
                         &extended,
                         &srv_status,
                         NULL, /* srv_domain */
                         &roam_status,
                         NULL, /* sim_state */
                         &sys_mode,
                         NULL, /* sys_submode_valid */
                         NULL, /* sys_submode */
                         NULL)) {
        /* If error, leave superclass' reg state alone if ^SYSINFO isn't supported.
         * NOTE: always complete NOT in idle here */
        g_simple_async_result_set_op_res_gpointer (ctx->result, &ctx->state, NULL);
        detailed_registration_state_context_complete_and_free (ctx);
        return;
    }

    if (srv_status == 2) {
        MMModemCdmaRegistrationState reg_state = MM_MODEM_CDMA_REGISTRATION_STATE_REGISTERED;
        MMModemAccessTechnology act;
        gboolean cdma1x = FALSE;
        gboolean evdo = FALSE;

        /* Service available, check roaming state */
        if (roam_status == 0)
            reg_state = MM_MODEM_CDMA_REGISTRATION_STATE_HOME;
        else if (roam_status == 1)
            reg_state = MM_MODEM_CDMA_REGISTRATION_STATE_ROAMING;

        /* Check service type */
        act = (extended ?
               huawei_sysinfoex_mode_to_act (sys_mode):
               huawei_sysinfo_mode_to_act (sys_mode));

        if (act & MM_MODEM_ACCESS_TECHNOLOGY_1XRTT) {
            cdma1x = TRUE;
            ctx->state.detailed_cdma1x_state = reg_state;
        }

        if (act & MM_MODEM_ACCESS_TECHNOLOGY_EVDO0 ||
            act & MM_MODEM_ACCESS_TECHNOLOGY_EVDOA ||
            act & MM_MODEM_ACCESS_TECHNOLOGY_EVDOB) {
            evdo = TRUE;
            ctx->state.detailed_evdo_state = reg_state;
        }

        if (!cdma1x && !evdo) {
            /* Say we're registered to something even though sysmode parsing failed */
            mm_dbg ("Assuming registered at least in CDMA1x");
            ctx->state.detailed_cdma1x_state = MM_MODEM_CDMA_REGISTRATION_STATE_REGISTERED;
        }
    }

    /* NOTE: always complete NOT in idle here */
    g_simple_async_result_set_op_res_gpointer (ctx->result, &ctx->state, NULL);
    detailed_registration_state_context_complete_and_free (ctx);
}

static void
get_detailed_registration_state (MMIfaceModemCdma *self,
                                 MMModemCdmaRegistrationState cdma1x_state,
                                 MMModemCdmaRegistrationState evdo_state,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
    DetailedRegistrationStateContext *ctx;

    /* Setup context */
    ctx = g_new0 (DetailedRegistrationStateContext, 1);
    ctx->self = g_object_ref (self);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             get_detailed_registration_state);
    ctx->state.detailed_cdma1x_state = cdma1x_state;
    ctx->state.detailed_evdo_state = evdo_state;

    sysinfo (MM_BROADBAND_MODEM_HUAWEI (self),
             (GAsyncReadyCallback)registration_state_sysinfo_ready,
             ctx);
}

/*****************************************************************************/
/* Load network time (Time interface) */

static MMNetworkTimezone *
modem_time_load_network_timezone_finish (MMIfaceModemTime *_self,
                                         GAsyncResult *res,
                                         GError **error)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);
    MMNetworkTimezone *tz = NULL;
    const gchar *response;

    g_assert (self->priv->nwtime_support == FEATURE_SUPPORTED ||
              self->priv->time_support == FEATURE_SUPPORTED);

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (_self), res, error);
    if (!response)
        return NULL;

    if (self->priv->nwtime_support == FEATURE_SUPPORTED)
        mm_huawei_parse_nwtime_response (response, NULL, &tz, error);
    else if (self->priv->time_support == FEATURE_SUPPORTED)
        mm_huawei_parse_time_response (response, NULL, &tz, error);
    return tz;
}


static gchar *
modem_time_load_network_time_finish (MMIfaceModemTime *_self,
                                     GAsyncResult *res,
                                     GError **error)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);
    const gchar *response;
    gchar *iso8601 = NULL;

    g_assert (self->priv->nwtime_support == FEATURE_SUPPORTED ||
              self->priv->time_support == FEATURE_SUPPORTED);

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (_self), res, error);
    if (!response)
        return NULL;

    if (self->priv->nwtime_support == FEATURE_SUPPORTED)
        mm_huawei_parse_nwtime_response (response, &iso8601, NULL, error);
    else if (self->priv->time_support == FEATURE_SUPPORTED)
        mm_huawei_parse_time_response (response, &iso8601, NULL, error);
    return iso8601;
}

static void
modem_time_load_network_time_or_zone (MMIfaceModemTime *_self,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
    const char *command = NULL;
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);

    if (self->priv->nwtime_support == FEATURE_SUPPORTED)
        command = "^NWTIME?";
    else if (self->priv->time_support == FEATURE_SUPPORTED)
        command = "^TIME";

    g_assert (command != NULL);

    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              command,
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* Power state loading (Modem interface) */

static void
enable_disable_unsolicited_rfswitch_event_handler (MMBroadbandModemHuawei *self,
                                                   gboolean enable)
{
    GList *ports, *l;

    ports = get_at_port_list (self);

    mm_dbg ("%s ^RFSWITCH unsolicited event handler",
            enable ? "Enable" : "Disable");

    for (l = ports; l; l = g_list_next (l)) {
        MMPortSerialAt *port = MM_PORT_SERIAL_AT (l->data);

        mm_port_serial_at_enable_unsolicited_msg_handler (
            port,
            self->priv->rfswitch_regex,
            enable);
    }

    g_list_free_full (ports, (GDestroyNotify)g_object_unref);
}

static void
parent_load_power_state_ready (MMIfaceModem *self,
                               GAsyncResult *res,
                               GSimpleAsyncResult *result)
{
    GError *error = NULL;
    MMModemPowerState power_state;

    power_state = iface_modem_parent->load_power_state_finish (self, res, &error);
    if (error)
        g_simple_async_result_take_error (result, error);
    else {
        /* As modem_power_down uses +CFUN=0 to put the modem in low state, we treat
         * CFUN 0 as 'LOW' power state instead of 'OFF'. Otherwise, MMIfaceModem
         * would prevent the modem from transitioning back to the 'ON' power state. */
        if (power_state == MM_MODEM_POWER_STATE_OFF)
            power_state = MM_MODEM_POWER_STATE_LOW;

        g_simple_async_result_set_op_res_gpointer (result, GUINT_TO_POINTER (power_state), NULL);
    }

    g_simple_async_result_complete (result);
    g_object_unref (result);
}

static void
huawei_rfswitch_check_ready (MMBaseModem *_self,
                             GAsyncResult *res,
                             GSimpleAsyncResult *result)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);
    GError *error = NULL;
    const gchar *response;
    gint sw_state;

    enable_disable_unsolicited_rfswitch_event_handler (MM_BROADBAND_MODEM_HUAWEI (self),
                                                       TRUE /* enable */);

    response = mm_base_modem_at_command_finish (_self, res, &error);
    if (response) {
        response = mm_strip_tag (response, "^RFSWITCH:");
        if (sscanf (response, "%d", &sw_state) != 1 ||
            (sw_state != 0 && sw_state != 1)) {
            mm_warn ("Couldn't parse ^RFSWITCH response: '%s'", response);
            error = g_error_new (MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Couldn't parse ^RFSWITCH response: '%s'",
                                 response);
        }
    }

    switch (self->priv->rfswitch_support) {
    case FEATURE_SUPPORT_UNKNOWN:
        if (error) {
            mm_dbg ("The device does not support ^RFSWITCH");
            self->priv->rfswitch_support = FEATURE_NOT_SUPPORTED;
            g_error_free (error);
            /* Fall back to parent's load_power_state */
            iface_modem_parent->load_power_state (MM_IFACE_MODEM (self),
                                                  (GAsyncReadyCallback)parent_load_power_state_ready,
                                                  result);
            return;
        }

        mm_dbg ("The device supports ^RFSWITCH");
        self->priv->rfswitch_support = FEATURE_SUPPORTED;
        break;
    case FEATURE_SUPPORTED:
        break;
    default:
        g_assert_not_reached ();
        break;
    }

    if (error)
        g_simple_async_result_take_error (result, error);
    else
        g_simple_async_result_set_op_res_gpointer (result,
                                                   sw_state ?
                                                   GUINT_TO_POINTER (MM_MODEM_POWER_STATE_ON) :
                                                   GUINT_TO_POINTER (MM_MODEM_POWER_STATE_LOW),
                                                   NULL);

    g_simple_async_result_complete (result);
    g_object_unref (result);
}

static MMModemPowerState
load_power_state_finish (MMIfaceModem *self,
                         GAsyncResult *res,
                         GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return MM_MODEM_POWER_STATE_UNKNOWN;

    return (MMModemPowerState)GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
}

static void
load_power_state (MMIfaceModem *self,
                  GAsyncReadyCallback callback,
                  gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_power_state);

    switch (MM_BROADBAND_MODEM_HUAWEI (self)->priv->rfswitch_support) {
    case FEATURE_SUPPORT_UNKNOWN:
    case FEATURE_SUPPORTED: {
        /* Temporarily disable the unsolicited ^RFSWITCH event handler in order to
         * prevent it from discarding the response to the ^RFSWITCH? command.
         * It will be re-enabled in huawei_rfswitch_check_ready.
         */
        enable_disable_unsolicited_rfswitch_event_handler (MM_BROADBAND_MODEM_HUAWEI (self),
                                                           FALSE /* enable */);
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "^RFSWITCH?",
                                  3,
                                  FALSE,
                                  (GAsyncReadyCallback)huawei_rfswitch_check_ready,
                                  result);
        break;
    }
    case FEATURE_NOT_SUPPORTED:
      /* Run parent's load_power_state */
      iface_modem_parent->load_power_state (self,
                                            (GAsyncReadyCallback)parent_load_power_state_ready,
                                            result);
      break;
    default:
      g_assert_not_reached ();
      break;
    }
}

/*****************************************************************************/
/* Modem power up (Modem interface) */

static gboolean
huawei_modem_power_up_finish (MMIfaceModem *self,
                              GAsyncResult *res,
                              GError **error)
{
    return !!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
}

static void
huawei_modem_power_up (MMIfaceModem *self,
                       GAsyncReadyCallback callback,
                       gpointer user_data)
{
    switch (MM_BROADBAND_MODEM_HUAWEI (self)->priv->rfswitch_support) {
    case FEATURE_NOT_SUPPORTED:
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "+CFUN=1",
                                  30,
                                  FALSE,
                                  callback,
                                  user_data);
        break;
    case FEATURE_SUPPORTED:
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "^RFSWITCH=1",
                                  30,
                                  FALSE,
                                  callback,
                                  user_data);
        break;
    default:
        g_assert_not_reached ();
        break;
    }
}

/*****************************************************************************/
/* Modem power down (Modem interface) */

static gboolean
huawei_modem_power_down_finish (MMIfaceModem *self,
                                GAsyncResult *res,
                                GError **error)
{
    return !!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
}

static void
huawei_modem_power_down (MMIfaceModem *self,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
    switch (MM_BROADBAND_MODEM_HUAWEI (self)->priv->rfswitch_support) {
    case FEATURE_NOT_SUPPORTED:
        /* +CFUN=0 is supported on all Huawei modems but +CFUN=4 isn't,
         * thus we use +CFUN=0 to put the modem in low power state. */
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "+CFUN=0",
                                  30,
                                  FALSE,
                                  callback,
                                  user_data);
        break;
    case FEATURE_SUPPORTED:
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "^RFSWITCH=0",
                                  30,
                                  FALSE,
                                  callback,
                                  user_data);
        break;
    default:
        g_assert_not_reached ();
        break;
    }
}

/*****************************************************************************/
/* Create SIM (Modem interface) */

static MMBaseSim *
huawei_modem_create_sim_finish (MMIfaceModem *self,
                                GAsyncResult *res,
                                GError **error)
{
    return mm_sim_huawei_new_finish (res, error);
}

static void
huawei_modem_create_sim (MMIfaceModem *self,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
    /* New Sierra SIM */
    mm_sim_huawei_new (MM_BASE_MODEM (self),
                       NULL, /* cancellable */
                       callback,
                       user_data);
}


/*****************************************************************************/
/* Location capabilities loading (Location interface) */

static MMModemLocationSource
location_load_capabilities_finish (MMIfaceModemLocation *self,
                                   GAsyncResult *res,
                                   GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return MM_MODEM_LOCATION_SOURCE_NONE;

    return (MMModemLocationSource) GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
}

static void
parent_load_capabilities_ready (MMIfaceModemLocation *self,
                                GAsyncResult *res,
                                GSimpleAsyncResult *simple)
{
    MMModemLocationSource sources;
    GError *error = NULL;

    sources = iface_modem_location_parent->load_capabilities_finish (self, res, &error);
    if (error) {
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    /* not sure how to check if GPS is supported, just allow it */
    if (mm_base_modem_peek_port_gps (MM_BASE_MODEM (self)))
        sources |= (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                    MM_MODEM_LOCATION_SOURCE_GPS_RAW  |
                    MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED);

    /* So we're done, complete */
    g_simple_async_result_set_op_res_gpointer (simple,
                                               GUINT_TO_POINTER (sources),
                                               NULL);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
location_load_capabilities (MMIfaceModemLocation *self,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        location_load_capabilities);

    /* Chain up parent's setup */
    iface_modem_location_parent->load_capabilities (self,
                                                    (GAsyncReadyCallback)parent_load_capabilities_ready,
                                                    result);
}

/*****************************************************************************/
/* Enable/Disable location gathering (Location interface) */

typedef struct {
    MMBroadbandModemHuawei *self;
    GSimpleAsyncResult *result;
    MMModemLocationSource source;
    int idx;
} LocationGatheringContext;

static void
location_gathering_context_complete_and_free (LocationGatheringContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->self);
    g_slice_free (LocationGatheringContext, ctx);
}

/******************************/
/* Disable location gathering */

static gboolean
disable_location_gathering_finish (MMIfaceModemLocation *self,
                                   GAsyncResult *res,
                                   GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
gps_disabled_ready (MMBaseModem *self,
                    GAsyncResult *res,
                    LocationGatheringContext *ctx)
{
    MMPortSerialGps *gps_port;
    GError *error = NULL;

    if (!mm_base_modem_at_command_full_finish (self, res, &error))
        g_simple_async_result_take_error (ctx->result, error);
    else
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);

    /* Only use the GPS port in NMEA/RAW setups */
    if (ctx->source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                       MM_MODEM_LOCATION_SOURCE_GPS_RAW)) {
        /* Even if we get an error here, we try to close the GPS port */
        gps_port = mm_base_modem_peek_port_gps (self);
        if (gps_port)
            mm_port_serial_close (MM_PORT_SERIAL (gps_port));
    }

    location_gathering_context_complete_and_free (ctx);
}

static void
disable_location_gathering (MMIfaceModemLocation *_self,
                            MMModemLocationSource source,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);
    gboolean stop_gps = FALSE;
    LocationGatheringContext *ctx;

    ctx = g_slice_new (LocationGatheringContext);
    ctx->self = g_object_ref (self);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             disable_location_gathering);
    ctx->source = source;

    /* Only stop GPS engine if no GPS-related sources enabled */
    if (source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                  MM_MODEM_LOCATION_SOURCE_GPS_RAW |
                  MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)) {
        self->priv->enabled_sources &= ~source;

        if (!(self->priv->enabled_sources & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                                             MM_MODEM_LOCATION_SOURCE_GPS_RAW |
                                             MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)))
            stop_gps = TRUE;
    }

    if (stop_gps) {
        mm_base_modem_at_command_full (MM_BASE_MODEM (_self),
                                       mm_base_modem_peek_port_primary (MM_BASE_MODEM (_self)),
                                       "^WPEND",
                                       3,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)gps_disabled_ready,
                                       ctx);
        return;
    }

    /* For any other location (e.g. 3GPP), or if still some GPS needed, just return */
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    location_gathering_context_complete_and_free (ctx);
}

/*****************************************************************************/
/* Enable location gathering (Location interface) */

static gboolean
enable_location_gathering_finish (MMIfaceModemLocation *self,
                                  GAsyncResult *res,
                                  GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static char *gps_startup[] = {
    "^WPDOM=0",
    "^WPDST=1",
    "^WPDFR=65535,30",
    "^WPDGP",
    NULL
};

static void
gps_enabled_ready (MMBaseModem *self,
                   GAsyncResult *res,
                   LocationGatheringContext *ctx)
{
    GError *error = NULL;
    MMPortSerialGps *gps_port;

    if (!mm_base_modem_at_command_full_finish (self, res, &error)) {
        ctx->idx = 0;
        g_simple_async_result_take_error (ctx->result, error);
        location_gathering_context_complete_and_free (ctx);
        return;
    }

    /* ctx->idx++; make sure ctx->idx is a valid command */
    if (gps_startup[ctx->idx++] && gps_startup[ctx->idx]) {
       mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                      mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
                                      gps_startup[ctx->idx],
                                      3,
                                      FALSE,
                                      FALSE, /* raw */
                                      NULL, /* cancellable */
                                      (GAsyncReadyCallback)gps_enabled_ready,
                                      ctx);
       return;
    }

    /* Only use the GPS port in NMEA/RAW setups */
    if (ctx->source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                       MM_MODEM_LOCATION_SOURCE_GPS_RAW)) {
        gps_port = mm_base_modem_peek_port_gps (self);
        if (!gps_port ||
            !mm_port_serial_open (MM_PORT_SERIAL (gps_port), &error)) {
            if (error)
                g_simple_async_result_take_error (ctx->result, error);
            else
                g_simple_async_result_set_error (ctx->result,
                                                 MM_CORE_ERROR,
                                                 MM_CORE_ERROR_FAILED,
                                                 "Couldn't open raw GPS serial port");
        } else
            g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    } else
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);

    location_gathering_context_complete_and_free (ctx);
}

static void
parent_enable_location_gathering_ready (MMIfaceModemLocation *self,
                                        GAsyncResult *res,
                                        LocationGatheringContext *ctx)
{
    gboolean start_gps = FALSE;
    GError *error = NULL;

    if (!iface_modem_location_parent->enable_location_gathering_finish (self, res, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        location_gathering_context_complete_and_free (ctx);
        return;
    }

    /* Now our own enabling */

    /* NMEA and RAW are both enabled in the same way */
    if (ctx->source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                       MM_MODEM_LOCATION_SOURCE_GPS_RAW |
                       MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)) {
        /* Only start GPS engine if not done already */
        if (!(ctx->self->priv->enabled_sources & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                                                  MM_MODEM_LOCATION_SOURCE_GPS_RAW |
                                                  MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)))
            start_gps = TRUE;
        ctx->self->priv->enabled_sources |= ctx->source;
    }

    if (start_gps) {
        mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                       mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
                                       gps_startup[ctx->idx],
                                       3,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)gps_enabled_ready,
                                       ctx);
        return;
    }

    /* For any other location (e.g. 3GPP), or if GPS already running just return */
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    location_gathering_context_complete_and_free (ctx);
}

static void
enable_location_gathering (MMIfaceModemLocation *self,
                           MMModemLocationSource source,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    LocationGatheringContext *ctx;

    ctx = g_slice_new (LocationGatheringContext);
    ctx->self = g_object_ref (self);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             enable_location_gathering);
    ctx->source = source;
    ctx->idx = 0;

    /* Chain up parent's gathering enable */
    iface_modem_location_parent->enable_location_gathering (self,
                                                            source,
                                                            (GAsyncReadyCallback)parent_enable_location_gathering_ready,
                                                            ctx);
}

/*****************************************************************************/
/* Check support (Time interface) */

static gboolean
modem_time_check_support_finish (MMIfaceModemTime *_self,
                                 GAsyncResult *res,
                                 GError **error)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);

    if (self->priv->nwtime_support == FEATURE_SUPPORTED)
        return TRUE;
    if (self->priv->time_support == FEATURE_SUPPORTED)
        return TRUE;
    return FALSE;
}

static void
modem_time_check_ready (MMBaseModem *self,
                        GAsyncResult *res,
                        GSimpleAsyncResult *simple)
{
    /* Responses are checked in the sequence parser, ignore overall result */
    mm_base_modem_at_sequence_finish (self, res, NULL, NULL);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static gboolean
modem_check_time_reply (MMBaseModem *_self,
                        gpointer none,
                        const gchar *command,
                        const gchar *response,
                        gboolean last_command,
                        const GError *error,
                        GVariant **result,
                        GError **result_error)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (_self);

    if (!error) {
        if (strstr (response, "^NTCT"))
            self->priv->nwtime_support = FEATURE_SUPPORTED;
        else if (strstr (response, "^TIME"))
            self->priv->time_support = FEATURE_SUPPORTED;
    } else {
        if (strstr (command, "^NTCT"))
            self->priv->nwtime_support = FEATURE_NOT_SUPPORTED;
        else if (strstr (command, "^TIME"))
            self->priv->time_support = FEATURE_NOT_SUPPORTED;
    }

    return FALSE;
}

static const MMBaseModemAtCommand time_cmd_sequence[] = {
    { "^NTCT?", 3, FALSE, modem_check_time_reply }, /* 3GPP/LTE */
    { "^TIME",  3, FALSE, modem_check_time_reply }, /* CDMA */
    { NULL }
};

static void
modem_time_check_support (MMIfaceModemTime *self,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_time_check_support);

    mm_base_modem_at_sequence (MM_BASE_MODEM (self),
                               time_cmd_sequence,
                               NULL, /* response_processor_context */
                               NULL, /* response_processor_context_free */
                               (GAsyncReadyCallback)modem_time_check_ready,
                               result);
}

/*****************************************************************************/
/* Setup ports (Broadband modem class) */

static void
set_ignored_unsolicited_events_handlers (MMBroadbandModemHuawei *self)
{
    GList *ports, *l;

    ports = get_at_port_list (self);

    /* Enable/disable unsolicited events in given port */
    for (l = ports; l; l = g_list_next (l)) {
        MMPortSerialAt *port = MM_PORT_SERIAL_AT (l->data);

        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->boot_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->connect_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->csnr_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->cusatp_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->cusatend_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->dsdormant_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->simst_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->srvst_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->stin_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->hcsq_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->pdpdeact_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->ndisend_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->rfswitch_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->position_regex,
            NULL, NULL, NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->posend_regex,
            NULL, NULL, NULL);
    }

    g_list_free_full (ports, (GDestroyNotify)g_object_unref);
}

static void
gps_trace_received (MMPortSerialGps *port,
                    const gchar *trace,
                    MMIfaceModemLocation *self)
{
    mm_iface_modem_location_gps_update (self, trace);
}

static void
setup_ports (MMBroadbandModem *self)
{
    MMPortSerialGps *gps_data_port;

    /* Call parent's setup ports first always */
    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_huawei_parent_class)->setup_ports (self);

    /* Unsolicited messages to always ignore */
    set_ignored_unsolicited_events_handlers (MM_BROADBAND_MODEM_HUAWEI (self));

    /* Now reset the unsolicited messages we'll handle when enabled */
    set_3gpp_unsolicited_events_handlers (MM_BROADBAND_MODEM_HUAWEI (self), FALSE);
    set_cdma_unsolicited_events_handlers (MM_BROADBAND_MODEM_HUAWEI (self), FALSE);

    /* NMEA GPS monitoring */
    gps_data_port = mm_base_modem_peek_port_gps (MM_BASE_MODEM (self));
    if (gps_data_port) {
        /* make sure GPS is stopped incase it was left enabled */
        mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                       mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
                                       "^WPEND",
                                       3, FALSE, FALSE, NULL, NULL, NULL);
        /* Add handler for the NMEA traces */
        mm_port_serial_gps_add_trace_handler (gps_data_port,
                                              (MMPortSerialGpsTraceFn)gps_trace_received,
                                              self, NULL);
    }
}

/*****************************************************************************/

MMBroadbandModemHuawei *
mm_broadband_modem_huawei_new (const gchar *device,
                               const gchar **drivers,
                               const gchar *plugin,
                               guint16 vendor_id,
                               guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_HUAWEI,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
mm_broadband_modem_huawei_init (MMBroadbandModemHuawei *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_BROADBAND_MODEM_HUAWEI,
                                              MMBroadbandModemHuaweiPrivate);
    /* Prepare regular expressions to setup */
    self->priv->rssi_regex = g_regex_new ("\\r\\n\\^RSSI:\\s*(\\d+)\\r\\n",
                                           G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->rssilvl_regex = g_regex_new ("\\r\\n\\^RSSILVL:\\s*(\\d+)\\r+\\n",
                                             G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->hrssilvl_regex = g_regex_new ("\\r\\n\\^HRSSILVL:\\s*(\\d+)\\r+\\n",
                                              G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);

    /* 3GPP: <cr><lf>^MODE:5<cr><lf>
     * CDMA: <cr><lf>^MODE: 2<cr><cr><lf>
     */
    self->priv->mode_regex = g_regex_new ("\\r\\n\\^MODE:\\s*(\\d*),?(\\d*)\\r+\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->dsflowrpt_regex = g_regex_new ("\\r\\n\\^DSFLOWRPT:(.+)\\r\\n",
                                               G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->ndisstat_regex = g_regex_new ("\\r\\n(\\^NDISSTAT:.+)\\r+\\n",
                                              G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->boot_regex = g_regex_new ("\\r\\n\\^BOOT:.+\\r\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->connect_regex = g_regex_new ("\\r\\n\\^CONNECT .+\\r\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->csnr_regex = g_regex_new ("\\r\\n\\^CSNR:.+\\r\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->cusatp_regex = g_regex_new ("\\r\\n\\+CUSATP:.+\\r\\n",
                                            G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->cusatend_regex = g_regex_new ("\\r\\n\\+CUSATEND\\r\\n",
                                              G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->dsdormant_regex = g_regex_new ("\\r\\n\\^DSDORMANT:.+\\r\\n",
                                               G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->simst_regex = g_regex_new ("\\r\\n\\^SIMST:.+\\r\\n",
                                           G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->srvst_regex = g_regex_new ("\\r\\n\\^SRVST:.+\\r\\n",
                                           G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->stin_regex = g_regex_new ("\\r\\n\\^STIN:.+\\r\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->hcsq_regex = g_regex_new ("\\r\\n\\^HCSQ:.+\\r+\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->pdpdeact_regex = g_regex_new ("\\r\\n\\^PDPDEACT:.+\\r+\\n",
                                              G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->ndisend_regex = g_regex_new ("\\r\\n\\^NDISEND:.+\\r+\\n",
                                             G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->rfswitch_regex = g_regex_new ("\\r\\n\\^RFSWITCH:.+\\r\\n",
                                              G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->position_regex = g_regex_new ("\\r\\n\\^POSITION:.+\\r\\n",
                                              G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->posend_regex = g_regex_new ("\\r\\n\\^POSEND:.+\\r\\n",
                                            G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);

    self->priv->ndisdup_support = FEATURE_SUPPORT_UNKNOWN;
    self->priv->rfswitch_support = FEATURE_SUPPORT_UNKNOWN;
    self->priv->sysinfoex_support = FEATURE_SUPPORT_UNKNOWN;
    self->priv->syscfg_support = FEATURE_SUPPORT_UNKNOWN;
    self->priv->syscfgex_support = FEATURE_SUPPORT_UNKNOWN;
    self->priv->prefmode_support = FEATURE_SUPPORT_UNKNOWN;
    self->priv->nwtime_support = FEATURE_SUPPORT_UNKNOWN;
    self->priv->time_support = FEATURE_SUPPORT_UNKNOWN;
}

static void
finalize (GObject *object)
{
    MMBroadbandModemHuawei *self = MM_BROADBAND_MODEM_HUAWEI (object);

    g_regex_unref (self->priv->rssi_regex);
    g_regex_unref (self->priv->rssilvl_regex);
    g_regex_unref (self->priv->hrssilvl_regex);
    g_regex_unref (self->priv->mode_regex);
    g_regex_unref (self->priv->dsflowrpt_regex);
    g_regex_unref (self->priv->ndisstat_regex);
    g_regex_unref (self->priv->boot_regex);
    g_regex_unref (self->priv->connect_regex);
    g_regex_unref (self->priv->csnr_regex);
    g_regex_unref (self->priv->cusatp_regex);
    g_regex_unref (self->priv->cusatend_regex);
    g_regex_unref (self->priv->dsdormant_regex);
    g_regex_unref (self->priv->simst_regex);
    g_regex_unref (self->priv->srvst_regex);
    g_regex_unref (self->priv->stin_regex);
    g_regex_unref (self->priv->hcsq_regex);
    g_regex_unref (self->priv->pdpdeact_regex);
    g_regex_unref (self->priv->ndisend_regex);
    g_regex_unref (self->priv->rfswitch_regex);
    g_regex_unref (self->priv->position_regex);
    g_regex_unref (self->priv->posend_regex);

    if (self->priv->syscfg_supported_modes)
        g_array_unref (self->priv->syscfg_supported_modes);
    if (self->priv->syscfgex_supported_modes)
        g_array_unref (self->priv->syscfgex_supported_modes);
    if (self->priv->prefmode_supported_modes)
        g_array_unref (self->priv->prefmode_supported_modes);

    G_OBJECT_CLASS (mm_broadband_modem_huawei_parent_class)->finalize (object);
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);

    iface->reset = reset;
    iface->reset_finish = reset_finish;
    iface->load_access_technologies = load_access_technologies;
    iface->load_access_technologies_finish = load_access_technologies_finish;
    iface->load_unlock_retries = load_unlock_retries;
    iface->load_unlock_retries_finish = load_unlock_retries_finish;
    iface->modem_after_sim_unlock = modem_after_sim_unlock;
    iface->modem_after_sim_unlock_finish = modem_after_sim_unlock_finish;
    iface->load_current_bands = load_current_bands;
    iface->load_current_bands_finish = load_current_bands_finish;
    iface->set_current_bands = set_current_bands;
    iface->set_current_bands_finish = set_current_bands_finish;
    iface->load_supported_modes = load_supported_modes;
    iface->load_supported_modes_finish = load_supported_modes_finish;
    iface->load_current_modes = load_current_modes;
    iface->load_current_modes_finish = load_current_modes_finish;
    iface->set_current_modes = set_current_modes;
    iface->set_current_modes_finish = set_current_modes_finish;
    iface->load_signal_quality = modem_load_signal_quality;
    iface->load_signal_quality_finish = modem_load_signal_quality_finish;
    iface->create_bearer = huawei_modem_create_bearer;
    iface->create_bearer_finish = huawei_modem_create_bearer_finish;
    iface->load_power_state = load_power_state;
    iface->load_power_state_finish = load_power_state_finish;
    iface->modem_power_up = huawei_modem_power_up;
    iface->modem_power_up_finish = huawei_modem_power_up_finish;
    iface->modem_power_down = huawei_modem_power_down;
    iface->modem_power_down_finish = huawei_modem_power_down_finish;
    iface->create_sim = huawei_modem_create_sim;
    iface->create_sim_finish = huawei_modem_create_sim_finish;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent (iface);

    iface->setup_unsolicited_events = modem_3gpp_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_3gpp_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
    iface->enable_unsolicited_events = modem_3gpp_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = modem_3gpp_enable_unsolicited_events_finish;
    iface->disable_unsolicited_events = modem_3gpp_disable_unsolicited_events;
    iface->disable_unsolicited_events_finish = modem_3gpp_disable_unsolicited_events_finish;
    iface->load_operator_name = modem_3gpp_load_operator_name;
    iface->load_operator_name_finish = modem_3gpp_load_operator_name_finish;
}

static void
iface_modem_3gpp_ussd_init (MMIfaceModem3gppUssd *iface)
{
    iface->encode = encode;
    iface->decode = decode;
}

static void
iface_modem_cdma_init (MMIfaceModemCdma *iface)
{
    iface_modem_cdma_parent = g_type_interface_peek_parent (iface);

    iface->setup_unsolicited_events = modem_cdma_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_cdma_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_cdma_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_cdma_setup_cleanup_unsolicited_events_finish;
    iface->setup_registration_checks = setup_registration_checks;
    iface->setup_registration_checks_finish = setup_registration_checks_finish;
    iface->get_detailed_registration_state = get_detailed_registration_state;
    iface->get_detailed_registration_state_finish = get_detailed_registration_state_finish;
}

static void
iface_modem_location_init (MMIfaceModemLocation *iface)
{
    iface_modem_location_parent = g_type_interface_peek_parent (iface);

    iface->load_capabilities = location_load_capabilities;
    iface->load_capabilities_finish = location_load_capabilities_finish;
    iface->enable_location_gathering = enable_location_gathering;
    iface->enable_location_gathering_finish = enable_location_gathering_finish;
    iface->disable_location_gathering = disable_location_gathering;
    iface->disable_location_gathering_finish = disable_location_gathering_finish;
}

static void
iface_modem_time_init (MMIfaceModemTime *iface)
{
    iface->check_support = modem_time_check_support;
    iface->check_support_finish = modem_time_check_support_finish;
    iface->load_network_time = modem_time_load_network_time_or_zone;
    iface->load_network_time_finish = modem_time_load_network_time_finish;
    iface->load_network_timezone = modem_time_load_network_time_or_zone;
    iface->load_network_timezone_finish = modem_time_load_network_timezone_finish;
}

static void
mm_broadband_modem_huawei_class_init (MMBroadbandModemHuaweiClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemHuaweiPrivate));

    object_class->finalize = finalize;

    broadband_modem_class->setup_ports = setup_ports;
}

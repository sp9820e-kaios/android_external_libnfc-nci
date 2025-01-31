/******************************************************************************
 *
 *  Copyright (C) 2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  Override the ALOGD(), ALOGE(), and other logging macros from
 *  /system/core/include/cutils/log.h
 *
 ******************************************************************************/
#include "OverrideLog.h"
#include <cutils/properties.h>
#include <string.h>
#include "config.h"
#include "android_logmsg.h"

#undef LOG_TAG
#if (NFC_SEC_NOT_OPEN_INCLUDED == TRUE) /* START_SLSI [S14111802] */
#define LOG_TAG "SecNfcJni"
#else
#define LOG_TAG "BrcmNfcJni"
#endif


/*******************************************************************************
**
** Function:        initializeGlobalAppLogLevel
**
** Description:     Initialize and get global logging level from .conf or
**                  Android property nfc.app_log_level.  The Android property
**                  overrides .conf variable.
**
** Returns:         Global log level:
**                  BT_TRACE_LEVEL_NONE    0        * No trace messages to be generated
**                  BT_TRACE_LEVEL_ERROR   1        * Error condition trace messages
**                  BT_TRACE_LEVEL_WARNING 2        * Warning condition trace messages
**                  BT_TRACE_LEVEL_API     3        * API traces
**                  BT_TRACE_LEVEL_EVENT   4        * Debug messages for events
**                  BT_TRACE_LEVEL_DEBUG   5        * Debug messages (general)
**
*******************************************************************************/
unsigned char initializeGlobalAppLogLevel ()
{
    unsigned long num = 0;
    char valueStr [PROPERTY_VALUE_MAX] = {0};

    num = 1;
    if (GetNumValue (NAME_APPL_TRACE_LEVEL, &num, sizeof(num)))
        appl_trace_level = (unsigned char) num;
    int len = property_get ("nfc.app_log_level", valueStr, "");
    if (len > 0)
    {
        //let Android property override .conf variable
        sscanf (valueStr, "%lu", &num);
        appl_trace_level = (unsigned char) num;
    }

    //0xFF is a special value used by the stack to query the current
    //trace level; it does not change any trace level
    if (appl_trace_level == 0xFF)
        appl_trace_level = BT_TRACE_LEVEL_DEBUG;
    ALOGD ("%s: level=%u", __FUNCTION__, appl_trace_level);

#if (NFC_SEC_NOT_OPEN_INCLUDED != TRUE) /* START_SLSI [S14111802] */
    if (appl_trace_level < BT_TRACE_LEVEL_DEBUG)
    {
        //display protocol traces in raw format
        ProtoDispAdapterUseRawOutput (TRUE);
    }
#endif
    return appl_trace_level;
}

UINT32 initializeProtocolLogLevel () {
    UINT32 num = 0;
    char valueStr [PROPERTY_VALUE_MAX] = {0};

    if ( GetNumValue ( NAME_PROTOCOL_TRACE_LEVEL, &num, sizeof ( num ) ) )
        ScrProtocolTraceFlag = num;

    int len = property_get ("nfc.enable_protocol_log", valueStr, "");
    if (len > 0)
    {
        if (strncmp("0", valueStr, 1) == 0)
        {
            ScrProtocolTraceFlag = 0;
        } else {
            ScrProtocolTraceFlag = ~0;
        }
    }

    return ScrProtocolTraceFlag;
}


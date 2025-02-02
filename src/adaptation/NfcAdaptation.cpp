/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
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
#include "OverrideLog.h"
#include "NfcAdaptation.h"
extern "C"
{
    #include "gki.h"
    #include "nfa_api.h"
    #include "nfc_int.h"
    #include "nfc_target.h"
    #include "vendor_cfg.h"
}
#include "config.h"
#include "android_logmsg.h"

#undef LOG_TAG
#define LOG_TAG "NfcAdaptation"

extern "C" void GKI_shutdown();
extern void resetConfig();
extern "C" void verify_stack_non_volatile_store ();
extern "C" void delete_stack_non_volatile_store (BOOLEAN forceDelete);

NfcAdaptation* NfcAdaptation::mpInstance = NULL;
ThreadMutex NfcAdaptation::sLock;
nfc_nci_device_t* NfcAdaptation::mHalDeviceContext = NULL;
tHAL_NFC_CBACK* NfcAdaptation::mHalCallback = NULL;
tHAL_NFC_DATA_CBACK* NfcAdaptation::mHalDataCallback = NULL;
ThreadCondVar NfcAdaptation::mHalOpenCompletedEvent;
ThreadCondVar NfcAdaptation::mHalCloseCompletedEvent;

UINT32 ScrProtocolTraceFlag = SCR_PROTO_TRACE_ALL; //0x017F00;
UINT8 appl_trace_level = 0xff;
char bcm_nfc_location[120];
char nci_hal_module[64];

static UINT8 nfa_dm_cfg[sizeof ( tNFA_DM_CFG ) ];
static UINT8 nfa_proprietary_cfg[sizeof ( tNFA_PROPRIETARY_CFG )];
extern tNFA_DM_CFG *p_nfa_dm_cfg;
extern tNFA_PROPRIETARY_CFG *p_nfa_proprietary_cfg;
extern UINT8 nfa_ee_max_ee_cfg;
extern const UINT8  nfca_version_string [];
extern const UINT8  nfa_version_string [];
static UINT8 deviceHostWhiteList [NFA_HCI_MAX_HOST_IN_NETWORK];
static tNFA_HCI_CFG jni_nfa_hci_cfg;
extern tNFA_HCI_CFG *p_nfa_hci_cfg;
extern BOOLEAN nfa_poll_bail_out_mode;

#if (NFC_SEC_NOT_OPEN_INCLUDED == TRUE)
tNFC_FW_VERSION NfcAdaptation::fw_version;  /* START_SLSI [S15052703] */
static bool firstInitRsp = true;                   /* START_SLSI [S14111804] */
#endif
/*******************************************************************************
**
** Function:    NfcAdaptation::NfcAdaptation()
**
** Description: class constructor
**
** Returns:     none
**
*******************************************************************************/
NfcAdaptation::NfcAdaptation()
{
    memset (&mHalEntryFuncs, 0, sizeof(mHalEntryFuncs));
}

/*******************************************************************************
**
** Function:    NfcAdaptation::~NfcAdaptation()
**
** Description: class destructor
**
** Returns:     none
**
*******************************************************************************/
NfcAdaptation::~NfcAdaptation()
{
    mpInstance = NULL;
}

/*******************************************************************************
**
** Function:    NfcAdaptation::GetInstance()
**
** Description: access class singleton
**
** Returns:     pointer to the singleton object
**
*******************************************************************************/
NfcAdaptation& NfcAdaptation::GetInstance()
{
    AutoThreadMutex  a(sLock);

    if (!mpInstance)
        mpInstance = new NfcAdaptation;
    return *mpInstance;
}

/*******************************************************************************
**
** Function:    NfcAdaptation::Initialize()
**
** Description: class initializer
**
** Returns:     none
**
*******************************************************************************/
void NfcAdaptation::Initialize ()
{
    const char* func = "NfcAdaptation::Initialize";
    ALOGD("%s: enter", func);
    ALOGE("%s: ver=%s nfa=%s", func, nfca_version_string, nfa_version_string);
    unsigned long num;

#if (NFC_SEC_NOT_OPEN_INCLUDED != TRUE) /* START_SLSI [S14111802] */
    if ( GetNumValue ( NAME_USE_RAW_NCI_TRACE, &num, sizeof ( num ) ) )
    {
        if (num == 1)
        {
            // display protocol traces in raw format
            ProtoDispAdapterUseRawOutput (TRUE);
            ALOGD("%s: logging protocol in raw format", func);
        }
    }
#endif
    if ( !GetStrValue ( NAME_NFA_STORAGE, bcm_nfc_location, sizeof ( bcm_nfc_location ) ) )
    {
        strlcpy (bcm_nfc_location, "/data/nfc", sizeof(bcm_nfc_location));
    }

    initializeProtocolLogLevel ();

    if ( GetStrValue ( NAME_NFA_DM_CFG, (char*)nfa_dm_cfg, sizeof ( nfa_dm_cfg ) ) )
        p_nfa_dm_cfg = ( tNFA_DM_CFG * ) ((void*)&nfa_dm_cfg[0]);

    nfa_ee_max_ee_cfg = NFA_EE_MAX_EE_SUPPORTED;
    ALOGD("%s: Overriding NFA_EE_MAX_EE_SUPPORTED to use %d", func, nfa_ee_max_ee_cfg);

    if ( GetNumValue ( NAME_NFA_POLL_BAIL_OUT_MODE, &num, sizeof ( num ) ) )
    {
        nfa_poll_bail_out_mode = num;
        ALOGD("%s: Overriding NFA_POLL_BAIL_OUT_MODE to use %d", func, nfa_poll_bail_out_mode);
    }

    if ( GetStrValue ( NAME_NFA_PROPRIETARY_CFG, (char*)nfa_proprietary_cfg, sizeof ( tNFA_PROPRIETARY_CFG ) ) )
    {
        p_nfa_proprietary_cfg = (tNFA_PROPRIETARY_CFG*)(void*)( &nfa_proprietary_cfg[0]);
    }
    //configure device host whitelist of HCI host ID's; see specification ETSI TS 102 622 V11.1.10
    //(2012-10), section 6.1.3.1
    num = GetStrValue ( NAME_DEVICE_HOST_WHITE_LIST, (char*) deviceHostWhiteList, sizeof ( deviceHostWhiteList ) );
    if (num)
    {
        memmove (&jni_nfa_hci_cfg, p_nfa_hci_cfg, sizeof(jni_nfa_hci_cfg));
        jni_nfa_hci_cfg.num_whitelist_host = (UINT8) num; //number of HCI host ID's in the whitelist
        jni_nfa_hci_cfg.p_whitelist = deviceHostWhiteList; //array of HCI host ID's
        p_nfa_hci_cfg = &jni_nfa_hci_cfg;
    }

    initializeGlobalAppLogLevel ();

    verify_stack_non_volatile_store ();
    if ( GetNumValue ( NAME_PRESERVE_STORAGE, (char*)&num, sizeof ( num ) ) &&
            (num == 1) )
        ALOGD ("%s: preserve stack NV store", __FUNCTION__);
    else
    {
        delete_stack_non_volatile_store (FALSE);
    }

    GKI_init ();
    GKI_enable ();
    GKI_create_task ((TASKPTR)NFCA_TASK, BTU_TASK, (INT8*)"NFCA_TASK", 0, 0, (pthread_cond_t*)NULL, NULL);
    {
        AutoThreadMutex guard(mCondVar);
        GKI_create_task ((TASKPTR)Thread, MMI_TASK, (INT8*)"NFCA_THREAD", 0, 0, (pthread_cond_t*)NULL, NULL);
        mCondVar.wait();
    }

    mHalDeviceContext = NULL;
    mHalCallback =  NULL;
    memset (&mHalEntryFuncs, 0, sizeof(mHalEntryFuncs));
    InitializeHalDeviceContext ();
    ALOGD ("%s: exit", func);
}

/*******************************************************************************
**
** Function:    NfcAdaptation::Finalize()
**
** Description: class finalizer
**
** Returns:     none
**
*******************************************************************************/
void NfcAdaptation::Finalize()
{
    const char* func = "NfcAdaptation::Finalize";
    AutoThreadMutex  a(sLock);

    ALOGD ("%s: enter", func);

#if(NFC_SEC_NOT_OPEN_INCLUDED == TRUE) /* START_SLSI [S14111804] */
    firstInitRsp = true;
#endif

    GKI_shutdown ();

    resetConfig();

    nfc_nci_close(mHalDeviceContext); //close the HAL's device context
    mHalDeviceContext = NULL;
    mHalCallback = NULL;
    memset (&mHalEntryFuncs, 0, sizeof(mHalEntryFuncs));

    ALOGD ("%s: exit", func);
    delete this;
}

/*******************************************************************************
**
** Function:    NfcAdaptation::signal()
**
** Description: signal the CondVar to release the thread that is waiting
**
** Returns:     none
**
*******************************************************************************/
void NfcAdaptation::signal ()
{
    mCondVar.signal();
}

/*******************************************************************************
**
** Function:    NfcAdaptation::NFCA_TASK()
**
** Description: NFCA_TASK runs the GKI main task
**
** Returns:     none
**
*******************************************************************************/
UINT32 NfcAdaptation::NFCA_TASK (UINT32 arg)
{
    const char* func = "NfcAdaptation::NFCA_TASK";
    (void)(arg);
    ALOGD ("%s: enter", func);
    GKI_run (0);
    ALOGD ("%s: exit", func);
    return 0;
}

/*******************************************************************************
**
** Function:    NfcAdaptation::Thread()
**
** Description: Creates work threads
**
** Returns:     none
**
*******************************************************************************/
UINT32 NfcAdaptation::Thread (UINT32 arg)
{
    const char* func = "NfcAdaptation::Thread";
    (void)(arg);
    ALOGD ("%s: enter", func);

    {
        ThreadCondVar    CondVar;
        AutoThreadMutex  guard(CondVar);
        GKI_create_task ((TASKPTR)nfc_task, NFC_TASK, (INT8*)"NFC_TASK", 0, 0, (pthread_cond_t*)CondVar, (pthread_mutex_t*)CondVar);
        CondVar.wait();
    }

    NfcAdaptation::GetInstance().signal();

    GKI_exit_task (GKI_get_taskid ());
    ALOGD ("%s: exit", func);
    return 0;
}

/*******************************************************************************
**
** Function:    NfcAdaptation::GetHalEntryFuncs()
**
** Description: Get the set of HAL entry points.
**
** Returns:     Functions pointers for HAL entry points.
**
*******************************************************************************/
tHAL_NFC_ENTRY* NfcAdaptation::GetHalEntryFuncs ()
{
    return &mHalEntryFuncs;
}

/*******************************************************************************
**
** Function:    NfcAdaptation::InitializeHalDeviceContext
**
** Description: Ask the generic Android HAL to find the Broadcom-specific HAL.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::InitializeHalDeviceContext ()
{
    const char* func = "NfcAdaptation::InitializeHalDeviceContext";
    ALOGD ("%s: enter", func);
    int ret = 0; //0 means success
#if(NFC_SEC_NOT_OPEN_INCLUDED != TRUE)
    if ( !GetStrValue ( NAME_NCI_HAL_MODULE, nci_hal_module, sizeof ( nci_hal_module) ) )
    {
        ALOGE("No HAL module specified in config, falling back to BCM2079x");
    strlcpy (nci_hal_module, "nfc_nci.bcm2079x", sizeof(nci_hal_module));
    }
#endif
    const hw_module_t* hw_module = NULL;

    mHalEntryFuncs.initialize = HalInitialize;
    mHalEntryFuncs.terminate = HalTerminate;
    mHalEntryFuncs.open = HalOpen;
    mHalEntryFuncs.close = HalClose;
    mHalEntryFuncs.core_initialized = HalCoreInitialized;
    mHalEntryFuncs.write = HalWrite;
    mHalEntryFuncs.prediscover = HalPrediscover;
    mHalEntryFuncs.control_granted = HalControlGranted;
    mHalEntryFuncs.power_cycle = HalPowerCycle;
    mHalEntryFuncs.get_max_ee = HalGetMaxNfcee;

#if (NFC_SEC_NOT_OPEN_INCLUDED == TRUE)
    ALOGE("use NFC_NCI_HARDWARE_MODULE_ID");
    ret = hw_get_module (NFC_NCI_HARDWARE_MODULE_ID, &hw_module);
#else
    ret = hw_get_module (nci_hal_module, &hw_module);
#endif
    if (ret == 0)
    {
        ret = nfc_nci_open (hw_module, &mHalDeviceContext);
        if (ret != 0)
            ALOGE ("%s: nfc_nci_open fail", func);
    }
    else
        ALOGE ("%s: fail hw_get_module %s", func, nci_hal_module);
    ALOGD ("%s: exit", func);
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalInitialize
**
** Description: Not implemented because this function is only needed
**              within the HAL.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalInitialize ()
{
    const char* func = "NfcAdaptation::HalInitialize";
    ALOGD ("%s", func);
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalTerminate
**
** Description: Not implemented because this function is only needed
**              within the HAL.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalTerminate ()
{
    const char* func = "NfcAdaptation::HalTerminate";
    ALOGD ("%s", func);
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalOpen
**
** Description: Turn on controller, download firmware.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalOpen (tHAL_NFC_CBACK *p_hal_cback, tHAL_NFC_DATA_CBACK* p_data_cback)
{
    const char* func = "NfcAdaptation::HalOpen";
    ALOGD ("%s", func);
    if (mHalDeviceContext)
    {
        mHalCallback = p_hal_cback;
        mHalDataCallback = p_data_cback;
        mHalDeviceContext->open (mHalDeviceContext, HalDeviceContextCallback, HalDeviceContextDataCallback);
    }
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalClose
**
** Description: Turn off controller.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalClose ()
{
    const char* func = "NfcAdaptation::HalClose";
    ALOGD ("%s", func);
    if (mHalDeviceContext)
    {
        mHalDeviceContext->close (mHalDeviceContext);
    }
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalDeviceContextCallback
**
** Description: Translate generic Android HAL's callback into Broadcom-specific
**              callback function.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalDeviceContextCallback (nfc_event_t event, nfc_status_t event_status)
{
    const char* func = "NfcAdaptation::HalDeviceContextCallback";
    ALOGD ("%s: event=%u", func, event);
    if (mHalCallback)
        mHalCallback (event, (tHAL_NFC_STATUS) event_status);
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalDeviceContextDataCallback
**
** Description: Translate generic Android HAL's callback into Broadcom-specific
**              callback function.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalDeviceContextDataCallback (uint16_t data_len, uint8_t* p_data)
{
    const char* func = "NfcAdaptation::HalDeviceContextDataCallback";
    ALOGD ("%s: len=%u", func, data_len);
    if (mHalDataCallback)
        mHalDataCallback (data_len, p_data);
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalWrite
**
** Description: Write NCI message to the controller.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalWrite (UINT16 data_len, UINT8* p_data)
{
    const char* func = "NfcAdaptation::HalWrite";
    ALOGD ("%s", func);
    if (mHalDeviceContext)
    {
        mHalDeviceContext->write (mHalDeviceContext, data_len, p_data);
    }
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalCoreInitialized
**
** Description: Adjust the configurable parameters in the controller.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalCoreInitialized (UINT8* p_core_init_rsp_params)
{
    const char* func = "NfcAdaptation::HalCoreInitialized";
    ALOGD ("%s", func);
    if (mHalDeviceContext)
    {
        mHalDeviceContext->core_initialized (mHalDeviceContext, p_core_init_rsp_params);
    }
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalPrediscover
**
** Description:     Perform any vendor-specific pre-discovery actions (if needed)
**                  If any actions were performed TRUE will be returned, and
**                  HAL_PRE_DISCOVER_CPLT_EVT will notify when actions are
**                  completed.
**
** Returns:          TRUE if vendor-specific pre-discovery actions initialized
**                  FALSE if no vendor-specific pre-discovery actions are needed.
**
*******************************************************************************/
BOOLEAN NfcAdaptation::HalPrediscover ()
{
    const char* func = "NfcAdaptation::HalPrediscover";
    ALOGD ("%s", func);
    BOOLEAN retval = FALSE;

    if (mHalDeviceContext)
    {
        retval = mHalDeviceContext->pre_discover (mHalDeviceContext);
    }
    return retval;
}

/*******************************************************************************
**
** Function:        HAL_NfcControlGranted
**
** Description:     Grant control to HAL control for sending NCI commands.
**                  Call in response to HAL_REQUEST_CONTROL_EVT.
**                  Must only be called when there are no NCI commands pending.
**                  HAL_RELEASE_CONTROL_EVT will notify when HAL no longer
**                  needs control of NCI.
**
** Returns:         void
**
*******************************************************************************/
void NfcAdaptation::HalControlGranted ()
{
    const char* func = "NfcAdaptation::HalControlGranted";
    ALOGD ("%s", func);
    if (mHalDeviceContext)
    {
        mHalDeviceContext->control_granted (mHalDeviceContext);
    }
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalPowerCycle
**
** Description: Turn off and turn on the controller.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalPowerCycle ()
{
    const char* func = "NfcAdaptation::HalPowerCycle";
    ALOGD ("%s", func);
    if (mHalDeviceContext)
    {
        mHalDeviceContext->power_cycle (mHalDeviceContext);
    }
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalGetMaxNfcee
**
** Description: Turn off and turn on the controller.
**
** Returns:     None.
**
*******************************************************************************/
UINT8 NfcAdaptation::HalGetMaxNfcee()
{
    const char* func = "NfcAdaptation::HalGetMaxNfcee";
    UINT8 maxNfcee = 0;
    ALOGD ("%s", func);
    if (mHalDeviceContext)
    {
        // TODO maco call into HAL when we figure out binary compatibility.
        return nfa_ee_max_ee_cfg;

        //mHalDeviceContext->get_max_ee (mHalDeviceContext, &maxNfcee);
    }

    return maxNfcee;
}


/*******************************************************************************
**
** Function:    NfcAdaptation::DownloadFirmware
**
** Description: Download firmware patch files.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::DownloadFirmware ()
{
    const char* func = "NfcAdaptation::DownloadFirmware";
    ALOGD ("%s: enter", func);
    HalInitialize ();

    mHalOpenCompletedEvent.lock ();
    ALOGD ("%s: try open HAL", func);
    HalOpen (HalDownloadFirmwareCallback, HalDownloadFirmwareDataCallback);
    mHalOpenCompletedEvent.wait ();
    mHalOpenCompletedEvent.unlock();

    ALOGD ("%s: try close HAL", func);
    HalClose ();
    HalTerminate ();
    ALOGD ("%s: exit", func);
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalDownloadFirmwareCallback
**
** Description: Receive events from the HAL.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalDownloadFirmwareCallback (nfc_event_t event, nfc_status_t event_status)
{
    const char* func = "NfcAdaptation::HalDownloadFirmwareCallback";
    (void)(event_status);
    ALOGD ("%s: event=0x%X", func, event);
    switch (event)
    {
#if(NFC_SEC_NOT_OPEN_INCLUDED == TRUE) /* START_SLSI [S14111804] */
    case HAL_NFC_OPEN_CPLT_EVT:
        ALOGD ("%s: HAL_NFC_OPEN_CPLT_EVT", func);
        {
            UINT8 nci_reset[] = {0x20, 0x00, 0x01, 0x01};
            ALOGD ("%s: Send CORE_RESET_CMD", func);
            HalWrite(4, nci_reset); // CORE_RESET_CMD
        }
        break;
    case HAL_NFC_POST_INIT_CPLT_EVT:
        ALOGD ("%s: HAL_NFC_INIT_CPLT_EVT", func);
        mHalOpenCompletedEvent.signal ();
        break;
    case HAL_NFC_CLOSE_CPLT_EVT:
        ALOGD ("%s: HAL_NFC_CLOSE_CPLT_EVT", func);
        mHalCloseCompletedEvent.signal ();
        break;
#else
    case HAL_NFC_OPEN_CPLT_EVT:
        {
            ALOGD ("%s: HAL_NFC_OPEN_CPLT_EVT", func);
            mHalOpenCompletedEvent.signal ();
            break;
        }
    case HAL_NFC_CLOSE_CPLT_EVT:
        {
            ALOGD ("%s: HAL_NFC_CLOSE_CPLT_EVT", func);
            mHalCloseCompletedEvent.signal ();
            break;
        }
#endif
    }
}

/*******************************************************************************
**
** Function:    NfcAdaptation::HalDownloadFirmwareDataCallback
**
** Description: Receive data events from the HAL.
**
** Returns:     None.
**
*******************************************************************************/
void NfcAdaptation::HalDownloadFirmwareDataCallback (uint16_t data_len, uint8_t* p_data)
{
#if(NFC_SEC_NOT_OPEN_INCLUDED == TRUE) /* START_SLSI [S14111804] */
    const char* func = "NfcAdaptation::HalDownloadFirmwareDataCallback";
    ALOGD ("%s: len=%u", func, data_len);

    if (data_len < 3)
        return;

    UINT8 mt = p_data[0] & 0xE0;
    UINT8 gid = p_data[0] & 0x0F;
    UINT8 oid = p_data[1];
    UINT8 status = p_data[3];

    /* START_SLSI [S15052703] */
    UINT8 *p_ver = &p_data[(p_data[2]+3)-4];

    if (mt == 0x40) // NCI_RSP
    {
        if (gid == 0x00 && oid == 0x00) // CORE_RESET_RSP
        {
            if (status == 0x00) // STATUS_OK
            {
                UINT8 nci_init[] = {0x20, 0x01, 0x00};
                HalWrite(4, nci_init); // CORE_INIT_CMD
            }
        }
        else if (gid == 0x00 && oid == 0x01) // CORE_INIT_RSP
        {
            /* START_SLSI [S15052703] */
            if(status == 0x00)
            {
                fw_version.major_version = (p_ver[0]>>4)&0xF;
                fw_version.minor_version = p_ver[0] & 0xF;
                fw_version.build_info_high = p_ver[1];
                fw_version.build_info_low = p_ver[2];
            }
            ALOGD ("%s: is first init rsp: %d", func, firstInitRsp);
            if (firstInitRsp && status == 0x00) // STATUS_OK
            {
                HalCoreInitialized(p_data); // POST INIT
                firstInitRsp = false;
            }
        }
    }
#endif
}


/*******************************************************************************
**
** Function:    ThreadMutex::ThreadMutex()
**
** Description: class constructor
**
** Returns:     none
**
*******************************************************************************/
ThreadMutex::ThreadMutex()
{
    pthread_mutexattr_t mutexAttr;

    pthread_mutexattr_init(&mutexAttr);
    pthread_mutex_init(&mMutex, &mutexAttr);
    pthread_mutexattr_destroy(&mutexAttr);
}

/*******************************************************************************
**
** Function:    ThreadMutex::~ThreadMutex()
**
** Description: class destructor
**
** Returns:     none
**
*******************************************************************************/
ThreadMutex::~ThreadMutex()
{
    pthread_mutex_destroy(&mMutex);
}

/*******************************************************************************
**
** Function:    ThreadMutex::lock()
**
** Description: lock kthe mutex
**
** Returns:     none
**
*******************************************************************************/
void ThreadMutex::lock()
{
    pthread_mutex_lock(&mMutex);
}

/*******************************************************************************
**
** Function:    ThreadMutex::unblock()
**
** Description: unlock the mutex
**
** Returns:     none
**
*******************************************************************************/
void ThreadMutex::unlock()
{
    pthread_mutex_unlock(&mMutex);
}

/*******************************************************************************
**
** Function:    ThreadCondVar::ThreadCondVar()
**
** Description: class constructor
**
** Returns:     none
**
*******************************************************************************/
ThreadCondVar::ThreadCondVar()
{
    pthread_condattr_t CondAttr;

    pthread_condattr_init(&CondAttr);
    pthread_cond_init(&mCondVar, &CondAttr);

    pthread_condattr_destroy(&CondAttr);
}

/*******************************************************************************
**
** Function:    ThreadCondVar::~ThreadCondVar()
**
** Description: class destructor
**
** Returns:     none
**
*******************************************************************************/
ThreadCondVar::~ThreadCondVar()
{
    pthread_cond_destroy(&mCondVar);
}

/*******************************************************************************
**
** Function:    ThreadCondVar::wait()
**
** Description: wait on the mCondVar
**
** Returns:     none
**
*******************************************************************************/
void ThreadCondVar::wait()
{
    pthread_cond_wait(&mCondVar, *this);
    pthread_mutex_unlock(*this);
}

/*******************************************************************************
**
** Function:    ThreadCondVar::signal()
**
** Description: signal the mCondVar
**
** Returns:     none
**
*******************************************************************************/
void ThreadCondVar::signal()
{
    AutoThreadMutex  a(*this);
    pthread_cond_signal(&mCondVar);
}

/*******************************************************************************
**
** Function:    AutoThreadMutex::AutoThreadMutex()
**
** Description: class constructor, automatically lock the mutex
**
** Returns:     none
**
*******************************************************************************/
AutoThreadMutex::AutoThreadMutex(ThreadMutex &m)
    : mm(m)
{
    mm.lock();
}

/*******************************************************************************
**
** Function:    AutoThreadMutex::~AutoThreadMutex()
**
** Description: class destructor, automatically unlock the mutex
**
** Returns:     none
**
*******************************************************************************/
AutoThreadMutex::~AutoThreadMutex()
{
    mm.unlock();
}

/*
 * UA4FX_Driver.c — CoreAudio HAL AudioServerPlugIn for the EDIROL / Roland UA-4FX
 * (Advanced mode, VID 0x0582 PID 0x00A3) on modern macOS (Apple silicon & Intel).
 *
 * Object model
 *   kAudioObjectPlugInObject (1)  — the plug-in
 *   kObjectID_Device         (2)  — "EDIROL UA-4FX", present only while the device is attached
 *   kObjectID_Stream_Input   (3)  — 2 ch capture
 *   kObjectID_Stream_Output  (4)  — 2 ch playback
 *
 * Audio flows through UA4FX_USB.c; this file only implements the HAL protocol.
 * Streams are exposed to the HAL as 32-bit float (converted here to/from the
 * device's native 24-bit packed PCM). The sample rate is whatever the rear
 * switch of the unit selects (44.1 / 48 / 96 kHz); it cannot be changed over USB.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#include "UA4FX_USB.h"
#include "UA4FX_Log.h"

enum {
    kObjectID_PlugIn        = kAudioObjectPlugInObject,
    kObjectID_Device        = 2,
    kObjectID_Stream_Input  = 3,
    kObjectID_Stream_Output = 4,
    kObjectID_Volume_Output = 5,   /* software controls; give macOS a volume slider / media keys */
    kObjectID_Mute_Output   = 6,
    kObjectID_Volume_Input  = 7,
    kObjectID_Mute_Input    = 8
};
#define kVolume_MinDB (-60.0)
#define kVolume_MaxDB (0.0)

#define kDevice_Name          "EDIROL UA-4FX"
#define kDevice_Manufacturer  "Roland / EDIROL"
#define kDevice_ModelUID      "com.ua4fx.driver:UA-4FX"
#define kPlugIn_BundleID      "com.ua4fx.driver"
#define kChannels             2

/* ------------------------------------------------------------------------- */
/* state                                                                      */

static AudioServerPlugInHostRef gHost = NULL;
static ua4fx_engine_t *gEngine = NULL;
static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32  gIOCount = 0;
static atomic_bool gDevicePresent;
static Float64 gSampleRate = 48000.0;
static CFStringRef gDeviceUID = NULL;
static Boolean gInputStreamActive = true, gOutputStreamActive = true;

/* user parameters (set from the control app through custom properties) */
static _Atomic float gInputGain = 1.0f, gOutputGain = 1.0f;   /* linear, effective = trim + volume */
static double gInputTrimDB = 0.0, gOutputTrimDB = 7.0;        /* "base" gain, set from the control app */
static double gInputVolDB = 0.0, gOutputVolDB = 0.0;          /* macOS volume slider / media keys, kVolume_MinDB..kVolume_MaxDB */
static Boolean gInputMute = false, gOutputMute = false;
#define kTrim_MinDB (-24.0)
#define kTrim_MaxDB (24.0)
static ua4fx_config_t gPendingConfig;                          /* geometry waiting for PerformDeviceConfigurationChange */

/* Custom properties (kAudioServerPlugInCustomPropertyDataTypeCFPropertyList) on the device object */
#define kUA4FX_Property_Config  'uacf'   /* dict, read/write: framesPerXfer, xfersInFlight, inputGainDB, outputGainDB, inputMute, outputMute */
#define kUA4FX_Property_Stats   'uast'   /* dict, read-only: live engine statistics */
#define kUA4FX_Storage_Config   CFSTR("config")
#define kChangeAction_Geometry  1

/* forward */
static HRESULT  UA4FX_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG    UA4FX_AddRef(void *inDriver);
static ULONG    UA4FX_Release(void *inDriver);
static OSStatus UA4FX_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus UA4FX_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID);
static OSStatus UA4FX_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus UA4FX_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus UA4FX_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus UA4FX_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo);
static OSStatus UA4FX_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo);
static Boolean  UA4FX_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus UA4FX_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus UA4FX_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus UA4FX_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus UA4FX_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData);
static OSStatus UA4FX_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus UA4FX_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus UA4FX_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed);
static OSStatus UA4FX_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace);
static OSStatus UA4FX_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
static OSStatus UA4FX_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer);
static OSStatus UA4FX_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

static AudioServerPlugInDriverInterface gDriverInterface = {
    NULL,
    UA4FX_QueryInterface, UA4FX_AddRef, UA4FX_Release,
    UA4FX_Initialize, UA4FX_CreateDevice, UA4FX_DestroyDevice, UA4FX_AddDeviceClient, UA4FX_RemoveDeviceClient,
    UA4FX_PerformDeviceConfigurationChange, UA4FX_AbortDeviceConfigurationChange,
    UA4FX_HasProperty, UA4FX_IsPropertySettable, UA4FX_GetPropertyDataSize, UA4FX_GetPropertyData, UA4FX_SetPropertyData,
    UA4FX_StartIO, UA4FX_StopIO, UA4FX_GetZeroTimeStamp, UA4FX_WillDoIOOperation, UA4FX_BeginIOOperation, UA4FX_DoIOOperation, UA4FX_EndIOOperation
};
static AudioServerPlugInDriverInterface *gDriverInterfacePtr = &gDriverInterface;
static AudioServerPlugInDriverRef gDriverRef = &gDriverInterfacePtr;
static ULONG gRefCount = 0;

/* ------------------------------------------------------------------------- */
/* factory / COM                                                              */

void *UA4FX_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);
void *UA4FX_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return NULL;
}

static HRESULT UA4FX_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    if (inDriver != gDriverRef || !outInterface) return E_INVALIDARG;
    CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (!uuid) return E_INVALIDARG;
    Boolean ok = CFEqual(uuid, IUnknownUUID) || CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(uuid);
    if (!ok) { *outInterface = NULL; return E_NOINTERFACE; }
    ++gRefCount; *outInterface = gDriverRef; return S_OK;
}
static ULONG UA4FX_AddRef(void *inDriver)  { (void)inDriver; return ++gRefCount; }
static ULONG UA4FX_Release(void *inDriver) { (void)inDriver; if (gRefCount > 0) --gRefCount; return gRefCount; }

/* ------------------------------------------------------------------------- */
/* hot-plug                                                                   */

static void notify_device_list_changed(void) {
    if (!gHost) return;
    AudioObjectPropertyAddress addrs[2] = {
        { kAudioPlugInPropertyDeviceList,  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
        { kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }
    };
    gHost->PropertiesChanged(gHost, kObjectID_PlugIn, 2, addrs);
}

static void hotplug(void *ctx, bool arrived) {
    (void)ctx;
    pthread_mutex_lock(&gStateMutex);
    if (arrived) {
        gSampleRate = (Float64)ua4fx_engine_sample_rate(gEngine);
        if (gDeviceUID) CFRelease(gDeviceUID);
        const char *serial = ua4fx_engine_serial(gEngine);
        if (serial && serial[0]) gDeviceUID = CFStringCreateWithFormat(NULL, NULL, CFSTR("UA4FX:%s"), serial);
        else gDeviceUID = CFStringCreateWithFormat(NULL, NULL, CFSTR("UA4FX:loc%08x"), ua4fx_engine_location_id(gEngine));
        atomic_store(&gDevicePresent, true);
        LOG("HAL: device arrived, rate %.0f", gSampleRate);
    } else {
        atomic_store(&gDevicePresent, false);
        gIOCount = 0;
        LOG("HAL: device removed");
    }
    pthread_mutex_unlock(&gStateMutex);
    notify_device_list_changed();
}

/* ------------------------------------------------------------------------- */
/* configuration dictionaries                                                 */

static CFNumberRef num_i(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }
static CFNumberRef num_d(double v) { return CFNumberCreate(NULL, kCFNumberDoubleType, &v); }
static Boolean dict_get_d(CFDictionaryRef d, CFStringRef k, double *out) {
    CFTypeRef v = CFDictionaryGetValue(d, k);
    if (!v) return false;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) return CFNumberGetValue(v, kCFNumberDoubleType, out);
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) { *out = CFBooleanGetValue(v) ? 1 : 0; return true; }
    return false;
}
static float db_to_lin(double db) { return db <= -90.0 ? 0.0f : (float)pow(10.0, db / 20.0); }
static void apply_gains(void) {
    atomic_store(&gInputGain,  gInputMute  ? 0.0f : db_to_lin(gInputTrimDB  + gInputVolDB));
    atomic_store(&gOutputGain, gOutputMute ? 0.0f : db_to_lin(gOutputTrimDB + gOutputVolDB));
}
static void notify_controls(void) {
    if (!gHost) return;
    AudioObjectPropertyAddress lv[2] = { { kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                                         { kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
    AudioObjectPropertyAddress bv = { kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress cfg = { kUA4FX_Property_Config, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    gHost->PropertiesChanged(gHost, kObjectID_Volume_Output, 2, lv);
    gHost->PropertiesChanged(gHost, kObjectID_Volume_Input, 2, lv);
    gHost->PropertiesChanged(gHost, kObjectID_Mute_Output, 1, &bv);
    gHost->PropertiesChanged(gHost, kObjectID_Mute_Input, 1, &bv);
    gHost->PropertiesChanged(gHost, kObjectID_Device, 1, &cfg);
}
/* volume control mapping: scalar 0..1 <-> dB (kVolume_MinDB..kVolume_MaxDB), dB-linear taper */
static Float32 db_to_scalar(double db) { if (db <= kVolume_MinDB) return 0.0f; if (db >= kVolume_MaxDB) return 1.0f; return (Float32)((db - kVolume_MinDB) / (kVolume_MaxDB - kVolume_MinDB)); }
static double scalar_to_db(Float32 s) { if (s <= 0) return kVolume_MinDB; if (s >= 1) return kVolume_MaxDB; return kVolume_MinDB + (kVolume_MaxDB - kVolume_MinDB) * (double)s; }
static CFDictionaryRef copy_config_dict(void) {
    ua4fx_config_t c; ua4fx_engine_get_config(gEngine, &c);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef n;
    n = num_i((int)c.framesPerXfer); CFDictionarySetValue(d, CFSTR("framesPerXfer"), n); CFRelease(n);
    n = num_i((int)c.xfersInFlight); CFDictionarySetValue(d, CFSTR("xfersInFlight"), n); CFRelease(n);
    n = num_i((int)c.xfersInFlightOut); CFDictionarySetValue(d, CFSTR("xfersInFlightOut"), n); CFRelease(n);
    n = num_i((int)c.outputLeadMs); CFDictionarySetValue(d, CFSTR("outputLeadMs"), n); CFRelease(n);
    n = num_d(gInputTrimDB);   CFDictionarySetValue(d, CFSTR("inputTrimDB"), n);    CFRelease(n);
    n = num_d(gOutputTrimDB);  CFDictionarySetValue(d, CFSTR("outputTrimDB"), n);   CFRelease(n);
    n = num_d(gInputVolDB);    CFDictionarySetValue(d, CFSTR("inputVolumeDB"), n);  CFRelease(n);
    n = num_d(gOutputVolDB);   CFDictionarySetValue(d, CFSTR("outputVolumeDB"), n); CFRelease(n);
    n = num_d(gInputTrimDB + gInputVolDB);   CFDictionarySetValue(d, CFSTR("inputGainDB"), n);  CFRelease(n);   /* effective, informational */
    n = num_d(gOutputTrimDB + gOutputVolDB); CFDictionarySetValue(d, CFSTR("outputGainDB"), n); CFRelease(n);
    CFDictionarySetValue(d, CFSTR("inputMute"),  gInputMute  ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(d, CFSTR("outputMute"), gOutputMute ? kCFBooleanTrue : kCFBooleanFalse);
    return d;
}
/* Applies gains immediately; returns true if the USB geometry changed (needs a config change cycle). */
static Boolean apply_config_dict(CFDictionaryRef d) {
    double v;
    if (dict_get_d(d, CFSTR("inputTrimDB"), &v))    gInputTrimDB  = v < kTrim_MinDB ? kTrim_MinDB : v > kTrim_MaxDB ? kTrim_MaxDB : v;
    if (dict_get_d(d, CFSTR("outputTrimDB"), &v))   gOutputTrimDB = v < kTrim_MinDB ? kTrim_MinDB : v > kTrim_MaxDB ? kTrim_MaxDB : v;
    if (dict_get_d(d, CFSTR("inputVolumeDB"), &v))  gInputVolDB   = v < kVolume_MinDB ? kVolume_MinDB : v > kVolume_MaxDB ? kVolume_MaxDB : v;
    if (dict_get_d(d, CFSTR("outputVolumeDB"), &v)) gOutputVolDB  = v < kVolume_MinDB ? kVolume_MinDB : v > kVolume_MaxDB ? kVolume_MaxDB : v;
    if (dict_get_d(d, CFSTR("inputMute"), &v))    gInputMute  = v != 0;
    if (dict_get_d(d, CFSTR("outputMute"), &v))   gOutputMute = v != 0;
    apply_gains();
    ua4fx_config_t cur; ua4fx_engine_get_config(gEngine, &cur);
    ua4fx_config_t want = cur;
    if (dict_get_d(d, CFSTR("framesPerXfer"), &v)) want.framesPerXfer = (uint32_t)v;
    if (dict_get_d(d, CFSTR("xfersInFlight"), &v)) want.xfersInFlight = (uint32_t)v;
    if (dict_get_d(d, CFSTR("xfersInFlightOut"), &v)) want.xfersInFlightOut = (uint32_t)v;
    if (dict_get_d(d, CFSTR("outputLeadMs"), &v)) want.outputLeadMs = (uint32_t)v;
    if (want.framesPerXfer < 1) want.framesPerXfer = 1; if (want.framesPerXfer > UA4FX_MAX_FRAMES_PER_XFER) want.framesPerXfer = UA4FX_MAX_FRAMES_PER_XFER;
    if (want.xfersInFlight < 2) want.xfersInFlight = 2; if (want.xfersInFlight > UA4FX_MAX_XFERS_IN_FLIGHT) want.xfersInFlight = UA4FX_MAX_XFERS_IN_FLIGHT;
    if (want.xfersInFlightOut < 2) want.xfersInFlightOut = 2; if (want.xfersInFlightOut > UA4FX_MAX_XFERS_IN_FLIGHT) want.xfersInFlightOut = UA4FX_MAX_XFERS_IN_FLIGHT;
    if (want.outputLeadMs < 1) want.outputLeadMs = 1; if (want.outputLeadMs > want.xfersInFlightOut * want.framesPerXfer - 1) want.outputLeadMs = want.xfersInFlightOut * want.framesPerXfer - 1;
    if (want.framesPerXfer != cur.framesPerXfer || want.xfersInFlight != cur.xfersInFlight || want.xfersInFlightOut != cur.xfersInFlightOut || want.outputLeadMs != cur.outputLeadMs) { gPendingConfig = want; return true; }
    return false;
}
static void save_config(void) {
    if (!gHost) return;
    CFDictionaryRef d = copy_config_dict();
    gHost->WriteToStorage(gHost, kUA4FX_Storage_Config, d);
    CFRelease(d);
}
static CFDictionaryRef copy_stats_dict(void) {
    ua4fx_stats_t st; ua4fx_engine_get_stats(gEngine, &st);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef n;
#define PUT_I(k, v) do { n = num_i((int)(v)); CFDictionarySetValue(d, CFSTR(k), n); CFRelease(n); } while (0)
#define PUT_D(k, v) do { n = num_d((double)(v)); CFDictionarySetValue(d, CFSTR(k), n); CFRelease(n); } while (0)
    CFDictionarySetValue(d, CFSTR("present"), atomic_load(&gDevicePresent) ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(d, CFSTR("running"), st.running ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(d, CFSTR("inputActive"), st.inputActive ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(d, CFSTR("outputActive"), st.outputActive ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(d, CFSTR("captureMaster"), st.captureMaster ? kCFBooleanTrue : kCFBooleanFalse);
    PUT_D("sampleRate", gSampleRate);
    PUT_I("framesPerXfer", st.framesPerXfer); PUT_I("xfersInFlight", st.xfersInFlight); PUT_I("xfersInFlightOut", st.xfersInFlightOut); PUT_I("outputLeadMs", st.outputLeadMs);
    PUT_I("lateFills", st.lateFills); PUT_I("lateHarvests", st.lateHarvests);
    PUT_D("harvestLagMaxUs", st.harvestLagMaxUs); PUT_D("harvestedByPoll", st.harvestedByPoll); PUT_D("harvestedByCallback", st.harvestedByCallback);
    PUT_I("safetyOffsetInput", ua4fx_engine_safety_offset_input(gEngine));
    PUT_I("safetyOffsetOutput", ua4fx_engine_safety_offset_output(gEngine));
    PUT_D("rxFrames", st.rxFrames); PUT_D("txFrames", st.txFrames);
    PUT_D("rxPackets", st.rxPackets); PUT_D("txPackets", st.txPackets);
    PUT_D("rxErrors", st.rxErrors); PUT_D("txErrors", st.txErrors); PUT_D("resyncs", st.resyncs);
    PUT_D("measuredRate", st.measuredRate); PUT_D("rxPerBusFrame", st.rxPerBusFrame); PUT_D("outRate", st.outRate);
    PUT_I("feedbackError", st.feedbackError); PUT_I("packetsAdjusted", st.packetsAdjusted);
    PUT_I("snaps", st.snaps); PUT_D("maxCompletionLatencyUs", st.maxCompletionLatencyUs); PUT_I("lateCompletions", st.lateCompletions);
    CFDictionarySetValue(d, CFSTR("rtPolicyOK"), st.rtPolicyOK ? kCFBooleanTrue : kCFBooleanFalse);
    PUT_I("ioClients", gIOCount);
    CFDictionarySetValue(d, CFSTR("driverVersion"), CFSTR("0.5.7"));
#undef PUT_I
#undef PUT_D
    return d;
}

/* ------------------------------------------------------------------------- */
/* basic operations                                                           */

static OSStatus UA4FX_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    gEngine = ua4fx_engine_create();
    if (!gEngine) { LOGE("engine creation failed"); return kAudioHardwareUnspecifiedError; }
    ua4fx_engine_set_hotplug_callback(gEngine, hotplug, NULL);
    apply_gains();
    { CFPropertyListRef stored = NULL;
      if (gHost->CopyFromStorage(gHost, kUA4FX_Storage_Config, &stored) == 0 && stored) {
          if (CFGetTypeID(stored) == CFDictionaryGetTypeID()) {
              if (apply_config_dict(stored)) {
                  /* v0.2/v0.3 shipped 1 ms x 3 / x 5 with a shared depth; migrate to the split defaults */
                  if (gPendingConfig.framesPerXfer == 1 && gPendingConfig.xfersInFlight < 8) gPendingConfig.xfersInFlight = UA4FX_DEFAULT_XFERS_IN_FLIGHT;
                  if (!CFDictionaryContainsKey(stored, CFSTR("xfersInFlightOut"))) gPendingConfig.xfersInFlightOut = UA4FX_DEFAULT_XFERS_IN_FLIGHT_OUT;
                  if (!CFDictionaryContainsKey(stored, CFSTR("outputLeadMs"))) gPendingConfig.outputLeadMs = UA4FX_DEFAULT_OUTPUT_LEAD_MS;
                  ua4fx_engine_set_config(gEngine, &gPendingConfig);
              }
          }
          CFRelease(stored);
      } }
    /* the engine may already have found the device during creation */
    if (ua4fx_engine_device_present(gEngine) && !atomic_load(&gDevicePresent)) hotplug(NULL, true);
    LOG("HAL: plug-in initialized (device %s)", atomic_load(&gDevicePresent) ? "present" : "absent");
    return 0;
}

static OSStatus UA4FX_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID) {
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus UA4FX_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) {
    (void)inDriver; (void)inDeviceObjectID; return kAudioHardwareUnsupportedOperationError;
}
static OSStatus UA4FX_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo; return 0;
}
static OSStatus UA4FX_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo; return 0;
}
static OSStatus UA4FX_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo) {
    (void)inChangeInfo;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (inChangeAction == kChangeAction_Geometry) {
        /* the host has stopped IO on this device; the engine picks the new geometry up at the next start */
        pthread_mutex_lock(&gStateMutex);
        ua4fx_engine_set_config(gEngine, &gPendingConfig);
        pthread_mutex_unlock(&gStateMutex);
        LOG("HAL: geometry changed to %u ms, in x %u, out x %u, lead %u ms", gPendingConfig.framesPerXfer, gPendingConfig.xfersInFlight, gPendingConfig.xfersInFlightOut, gPendingConfig.outputLeadMs);
        save_config();
        AudioObjectPropertyAddress addrs[4] = {
            { kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput,  kAudioObjectPropertyElementMain },
            { kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain },
            { kUA4FX_Property_Config,           kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kUA4FX_Property_Stats,            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
        gHost->PropertiesChanged(gHost, kObjectID_Device, 4, addrs);
    }
    return 0;
}
static OSStatus UA4FX_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo; return 0;
}

/* ------------------------------------------------------------------------- */
/* property helpers                                                           */

static AudioStreamBasicDescription stream_format(void) {
    AudioStreamBasicDescription d;
    d.mSampleRate = gSampleRate;
    d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    d.mBytesPerPacket = kChannels * sizeof(Float32);
    d.mFramesPerPacket = 1;
    d.mBytesPerFrame = kChannels * sizeof(Float32);
    d.mChannelsPerFrame = kChannels;
    d.mBitsPerChannel = 32;
    d.mReserved = 0;
    return d;
}

static Boolean is_stream(AudioObjectID id) { return id == kObjectID_Stream_Input || id == kObjectID_Stream_Output; }

/* ------------------------------------------------------------------------- */
/* HasProperty                                                                */

static Boolean plugin_has(const AudioObjectPropertyAddress *a) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
    }
    return false;
}
static Boolean device_has(const AudioObjectPropertyAddress *a) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams: case kAudioObjectPropertyControlList: case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate: case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockAlgorithm: case kAudioDevicePropertyClockIsStable:
        case kAudioObjectPropertyCustomPropertyInfoList: case kUA4FX_Property_Config: case kUA4FX_Property_Stats:
            return true;
        case kAudioDevicePropertyPreferredChannelsForStereo: case kAudioDevicePropertyPreferredChannelLayout:
            return a->mScope == kAudioObjectPropertyScopeInput || a->mScope == kAudioObjectPropertyScopeOutput;
    }
    return false;
}
static Boolean stream_has(const AudioObjectPropertyAddress *a) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName: case kAudioObjectPropertyOwnedObjects:
        case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
    }
    return false;
}

static Boolean is_volume(AudioObjectID id) { return id == kObjectID_Volume_Output || id == kObjectID_Volume_Input; }
static Boolean is_mute(AudioObjectID id)   { return id == kObjectID_Mute_Output || id == kObjectID_Mute_Input; }
static Boolean is_control(AudioObjectID id) { return is_volume(id) || is_mute(id); }
static Boolean control_has(AudioObjectID id, const AudioObjectPropertyAddress *a) {
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioControlPropertyScope: case kAudioControlPropertyElement:
            return true;
        case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue: case kAudioLevelControlPropertyDecibelRange:
        case kAudioLevelControlPropertyConvertScalarToDecibels: case kAudioLevelControlPropertyConvertDecibelsToScalar:
            return is_volume(id);
        case kAudioBooleanControlPropertyValue:
            return is_mute(id);
    }
    return false;
}

static Boolean UA4FX_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress) {
    (void)inClientProcessID;
    if (inDriver != gDriverRef || !inAddress) return false;
    switch (inObjectID) {
        case kObjectID_PlugIn: return plugin_has(inAddress);
        case kObjectID_Device: return device_has(inAddress);
        case kObjectID_Stream_Input: case kObjectID_Stream_Output: return stream_has(inAddress);
        case kObjectID_Volume_Output: case kObjectID_Mute_Output: case kObjectID_Volume_Input: case kObjectID_Mute_Input: return control_has(inObjectID, inAddress);
    }
    return false;
}

static OSStatus UA4FX_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable) {
    if (inDriver != gDriverRef || !inAddress || !outIsSettable) return kAudioHardwareIllegalOperationError;
    if (!UA4FX_HasProperty(inDriver, inObjectID, inClientProcessID, inAddress)) return kAudioHardwareUnknownPropertyError;
    *outIsSettable = false;
    if (inObjectID == kObjectID_Device && (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate || inAddress->mSelector == kUA4FX_Property_Config)) *outIsSettable = true;
    if (is_stream(inObjectID) && (inAddress->mSelector == kAudioStreamPropertyIsActive || inAddress->mSelector == kAudioStreamPropertyVirtualFormat || inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)) *outIsSettable = true;
    if (is_volume(inObjectID) && (inAddress->mSelector == kAudioLevelControlPropertyScalarValue || inAddress->mSelector == kAudioLevelControlPropertyDecibelValue)) *outIsSettable = true;
    if (is_mute(inObjectID) && inAddress->mSelector == kAudioBooleanControlPropertyValue) *outIsSettable = true;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* GetPropertyData (size + data in one routine)                               */

#define REQUIRE(sz) do { if (outData && inDataSize < (sz)) return kAudioHardwareBadPropertySizeError; *outDataSize = (sz); if (!outData) return 0; } while (0)

static OSStatus get_property(AudioObjectID inObjectID, const AudioObjectPropertyAddress *a, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    Boolean present = atomic_load(&gDevicePresent);

    /* ---- plug-in ---- */
    if (inObjectID == kObjectID_PlugIn) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass:  REQUIRE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioObjectClassID; return 0;
            case kAudioObjectPropertyClass:      REQUIRE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioPlugInClassID; return 0;
            case kAudioObjectPropertyOwner:      REQUIRE(sizeof(AudioObjectID)); *(AudioObjectID *)outData = kAudioObjectUnknown; return 0;
            case kAudioObjectPropertyManufacturer: REQUIRE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(kDevice_Manufacturer); return 0;
            case kAudioPlugInPropertyResourceBundle: REQUIRE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(""); return 0;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList: {
                UInt32 n = present ? 1 : 0;
                UInt32 want = inDataSize / sizeof(AudioObjectID); if (want < n) n = want;
                *outDataSize = n * sizeof(AudioObjectID);
                if (!outData) { *outDataSize = present ? sizeof(AudioObjectID) : 0; return 0; }
                if (n) ((AudioObjectID *)outData)[0] = kObjectID_Device;
                return 0;
            }
            case kAudioPlugInPropertyTranslateUIDToDevice: {
                if (inQualifierDataSize != sizeof(CFStringRef) || !inQualifierData) return kAudioHardwareBadPropertySizeError;
                REQUIRE(sizeof(AudioObjectID));
                CFStringRef uid = *(const CFStringRef *)inQualifierData;
                *(AudioObjectID *)outData = (present && gDeviceUID && uid && CFEqual(uid, gDeviceUID)) ? kObjectID_Device : kAudioObjectUnknown;
                return 0;
            }
        }
        return kAudioHardwareUnknownPropertyError;
    }

    if (!present) return kAudioHardwareBadObjectError;

    /* ---- device ---- */
    if (inObjectID == kObjectID_Device) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass:  REQUIRE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioObjectClassID; return 0;
            case kAudioObjectPropertyClass:      REQUIRE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioDeviceClassID; return 0;
            case kAudioObjectPropertyOwner:      REQUIRE(sizeof(AudioObjectID)); *(AudioObjectID *)outData = kObjectID_PlugIn; return 0;
            case kAudioObjectPropertyName:       REQUIRE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(kDevice_Name); return 0;
            case kAudioObjectPropertyManufacturer: REQUIRE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(kDevice_Manufacturer); return 0;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams:
            case kAudioObjectPropertyControlList: {
                AudioObjectID ids[6]; UInt32 n = 0;
                Boolean in = a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeInput;
                Boolean out = a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeOutput;
                if (a->mSelector != kAudioObjectPropertyControlList) { if (in) ids[n++] = kObjectID_Stream_Input; if (out) ids[n++] = kObjectID_Stream_Output; }
                if (a->mSelector != kAudioDevicePropertyStreams) {
                    if (out) { ids[n++] = kObjectID_Volume_Output; ids[n++] = kObjectID_Mute_Output; }
                    if (in)  { ids[n++] = kObjectID_Volume_Input;  ids[n++] = kObjectID_Mute_Input; }
                }
                if (!outData) { *outDataSize = n * sizeof(AudioObjectID); return 0; }
                UInt32 want = inDataSize / sizeof(AudioObjectID); if (want < n) n = want;
                memcpy(outData, ids, n * sizeof(AudioObjectID)); *outDataSize = n * sizeof(AudioObjectID); return 0;
            }
            case kAudioDevicePropertyDeviceUID:  REQUIRE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFRetain(gDeviceUID); return 0;
            case kAudioDevicePropertyModelUID:   REQUIRE(sizeof(CFStringRef)); *(CFStringRef *)outData = CFSTR(kDevice_ModelUID); return 0;
            case kAudioDevicePropertyTransportType: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = kAudioDeviceTransportTypeUSB; return 0;
            case kAudioDevicePropertyRelatedDevices: {
                if (!outData) { *outDataSize = sizeof(AudioObjectID); return 0; }
                if (inDataSize < sizeof(AudioObjectID)) { *outDataSize = 0; return 0; }
                *(AudioObjectID *)outData = kObjectID_Device; *outDataSize = sizeof(AudioObjectID); return 0;
            }
            case kAudioDevicePropertyClockDomain: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = 0x058200A3; return 0;
            case kAudioDevicePropertyDeviceIsAlive: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = 1; return 0;
            case kAudioDevicePropertyDeviceIsRunning: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = gIOCount > 0 ? 1 : 0; return 0;
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = 1; return 0;
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = 1; return 0;
            case kAudioDevicePropertyLatency: REQUIRE(sizeof(UInt32));
                *(UInt32 *)outData = a->mScope == kAudioObjectPropertyScopeInput ? ua4fx_engine_latency_input(gEngine) : ua4fx_engine_latency_output(gEngine); return 0;
            case kAudioDevicePropertySafetyOffset: REQUIRE(sizeof(UInt32));
                *(UInt32 *)outData = a->mScope == kAudioObjectPropertyScopeInput ? ua4fx_engine_safety_offset_input(gEngine) : ua4fx_engine_safety_offset_output(gEngine); return 0;
            case kAudioDevicePropertyNominalSampleRate: REQUIRE(sizeof(Float64)); *(Float64 *)outData = gSampleRate; return 0;
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                if (!outData) { *outDataSize = sizeof(AudioValueRange); return 0; }
                if (inDataSize < sizeof(AudioValueRange)) { *outDataSize = 0; return 0; }
                AudioValueRange *r = (AudioValueRange *)outData; r->mMinimum = r->mMaximum = gSampleRate; *outDataSize = sizeof(AudioValueRange); return 0;
            }
            case kAudioDevicePropertyIsHidden: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = 0; return 0;
            case kAudioDevicePropertyZeroTimeStampPeriod: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = ua4fx_engine_zts_period(gEngine); return 0;
            case kAudioDevicePropertyClockAlgorithm: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = kAudioDeviceClockAlgorithm12PtMovingWindowAverage; return 0;
            case kAudioDevicePropertyClockIsStable: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = 1; return 0;
            case kAudioObjectPropertyCustomPropertyInfoList: {
                AudioServerPlugInCustomPropertyInfo info[2] = {
                    { kUA4FX_Property_Config, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                    { kUA4FX_Property_Stats,  kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone } };
                if (!outData) { *outDataSize = sizeof info; return 0; }
                UInt32 n = inDataSize / sizeof(AudioServerPlugInCustomPropertyInfo); if (n > 2) n = 2;
                memcpy(outData, info, n * sizeof(AudioServerPlugInCustomPropertyInfo)); *outDataSize = n * sizeof(AudioServerPlugInCustomPropertyInfo); return 0;
            }
            case kUA4FX_Property_Config: REQUIRE(sizeof(CFPropertyListRef)); *(CFPropertyListRef *)outData = copy_config_dict(); return 0;
            case kUA4FX_Property_Stats:  REQUIRE(sizeof(CFPropertyListRef)); *(CFPropertyListRef *)outData = copy_stats_dict(); return 0;
            case kAudioDevicePropertyPreferredChannelsForStereo: REQUIRE(2 * sizeof(UInt32)); ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = 2; return 0;
            case kAudioDevicePropertyPreferredChannelLayout: {
                UInt32 sz = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription);
                REQUIRE(sz);
                AudioChannelLayout *l = (AudioChannelLayout *)outData;
                l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions; l->mChannelBitmap = 0; l->mNumberChannelDescriptions = kChannels;
                for (UInt32 i = 0; i < kChannels; i++) { l->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Left + i; l->mChannelDescriptions[i].mChannelFlags = 0; l->mChannelDescriptions[i].mCoordinates[0] = l->mChannelDescriptions[i].mCoordinates[1] = l->mChannelDescriptions[i].mCoordinates[2] = 0; }
                return 0;
            }
        }
        return kAudioHardwareUnknownPropertyError;
    }

    /* ---- streams ---- */
    if (is_stream(inObjectID)) {
        Boolean isInput = inObjectID == kObjectID_Stream_Input;
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass:  REQUIRE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioObjectClassID; return 0;
            case kAudioObjectPropertyClass:      REQUIRE(sizeof(AudioClassID)); *(AudioClassID *)outData = kAudioStreamClassID; return 0;
            case kAudioObjectPropertyOwner:      REQUIRE(sizeof(AudioObjectID)); *(AudioObjectID *)outData = kObjectID_Device; return 0;
            case kAudioObjectPropertyName:       REQUIRE(sizeof(CFStringRef)); *(CFStringRef *)outData = isInput ? CFSTR("UA-4FX Input") : CFSTR("UA-4FX Output"); return 0;
            case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
            case kAudioStreamPropertyIsActive:   REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = (isInput ? gInputStreamActive : gOutputStreamActive) ? 1 : 0; return 0;
            case kAudioStreamPropertyDirection:  REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = isInput ? 1 : 0; return 0;
            case kAudioStreamPropertyTerminalType: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = isInput ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine; return 0;
            case kAudioStreamPropertyStartingChannel: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = 1; return 0;
            case kAudioStreamPropertyLatency:    REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = 0; return 0;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: REQUIRE(sizeof(AudioStreamBasicDescription)); *(AudioStreamBasicDescription *)outData = stream_format(); return 0;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                if (!outData) { *outDataSize = sizeof(AudioStreamRangedDescription); return 0; }
                if (inDataSize < sizeof(AudioStreamRangedDescription)) { *outDataSize = 0; return 0; }
                AudioStreamRangedDescription *r = (AudioStreamRangedDescription *)outData;
                r->mFormat = stream_format(); r->mSampleRateRange.mMinimum = r->mSampleRateRange.mMaximum = gSampleRate;
                *outDataSize = sizeof(AudioStreamRangedDescription); return 0;
            }
        }
        return kAudioHardwareUnknownPropertyError;
    }

    /* ---- controls ---- */
    if (is_control(inObjectID)) {
        Boolean isOut = inObjectID == kObjectID_Volume_Output || inObjectID == kObjectID_Mute_Output;
        double db = isOut ? gOutputVolDB : gInputVolDB;
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: REQUIRE(sizeof(AudioClassID)); *(AudioClassID *)outData = is_volume(inObjectID) ? kAudioLevelControlClassID : kAudioBooleanControlClassID; return 0;
            case kAudioObjectPropertyClass:     REQUIRE(sizeof(AudioClassID)); *(AudioClassID *)outData = is_volume(inObjectID) ? kAudioVolumeControlClassID : kAudioMuteControlClassID; return 0;
            case kAudioObjectPropertyOwner:     REQUIRE(sizeof(AudioObjectID)); *(AudioObjectID *)outData = kObjectID_Device; return 0;
            case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
            case kAudioControlPropertyScope:    REQUIRE(sizeof(AudioObjectPropertyScope)); *(AudioObjectPropertyScope *)outData = isOut ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput; return 0;
            case kAudioControlPropertyElement:  REQUIRE(sizeof(AudioObjectPropertyElement)); *(AudioObjectPropertyElement *)outData = kAudioObjectPropertyElementMain; return 0;
            case kAudioLevelControlPropertyScalarValue:  REQUIRE(sizeof(Float32)); *(Float32 *)outData = db_to_scalar(db); return 0;
            case kAudioLevelControlPropertyDecibelValue: REQUIRE(sizeof(Float32)); *(Float32 *)outData = (Float32)db; return 0;
            case kAudioLevelControlPropertyDecibelRange: REQUIRE(sizeof(AudioValueRange)); ((AudioValueRange *)outData)->mMinimum = kVolume_MinDB; ((AudioValueRange *)outData)->mMaximum = kVolume_MaxDB; return 0;
            case kAudioLevelControlPropertyConvertScalarToDecibels: REQUIRE(sizeof(Float32)); *(Float32 *)outData = (Float32)scalar_to_db(*(Float32 *)outData); return 0;
            case kAudioLevelControlPropertyConvertDecibelsToScalar: REQUIRE(sizeof(Float32)); *(Float32 *)outData = db_to_scalar(*(Float32 *)outData); return 0;
            case kAudioBooleanControlPropertyValue: REQUIRE(sizeof(UInt32)); *(UInt32 *)outData = (isOut ? gOutputMute : gInputMute) ? 1 : 0; return 0;
        }
        return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareBadObjectError;
}

static OSStatus UA4FX_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize) {
    (void)inClientProcessID;
    if (inDriver != gDriverRef || !inAddress || !outDataSize) return kAudioHardwareIllegalOperationError;
    return get_property(inObjectID, inAddress, inQualifierDataSize, inQualifierData, 0, outDataSize, NULL);
}

static OSStatus UA4FX_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)inClientProcessID;
    if (inDriver != gDriverRef || !inAddress || !outDataSize || !outData) return kAudioHardwareIllegalOperationError;
    return get_property(inObjectID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
}

static OSStatus UA4FX_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData) {
    (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef || !inAddress) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObjectID_Device && inAddress->mSelector == kUA4FX_Property_Config) {
        if (inDataSize != sizeof(CFPropertyListRef) || !inData) return kAudioHardwareBadPropertySizeError;
        CFPropertyListRef pl = *(const CFPropertyListRef *)inData;
        if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) return kAudioHardwareIllegalOperationError;
        pthread_mutex_lock(&gStateMutex);
        Boolean geometryChanged = apply_config_dict((CFDictionaryRef)pl);
        pthread_mutex_unlock(&gStateMutex);
        save_config();
        notify_controls();
        if (geometryChanged) gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, kChangeAction_Geometry, NULL);
        return 0;
    }
    if (is_control(inObjectID)) {
        Boolean isOut = inObjectID == kObjectID_Volume_Output || inObjectID == kObjectID_Mute_Output;
        pthread_mutex_lock(&gStateMutex);
        OSStatus err = 0;
        switch (inAddress->mSelector) {
            case kAudioLevelControlPropertyScalarValue:
                if (inDataSize != sizeof(Float32)) { err = kAudioHardwareBadPropertySizeError; break; }
                if (isOut) gOutputVolDB = scalar_to_db(*(const Float32 *)inData); else gInputVolDB = scalar_to_db(*(const Float32 *)inData);
                break;
            case kAudioLevelControlPropertyDecibelValue: {
                if (inDataSize != sizeof(Float32)) { err = kAudioHardwareBadPropertySizeError; break; }
                double db = *(const Float32 *)inData; if (db < kVolume_MinDB) db = kVolume_MinDB; if (db > kVolume_MaxDB) db = kVolume_MaxDB;
                if (isOut) gOutputVolDB = db; else gInputVolDB = db;
                break; }
            case kAudioBooleanControlPropertyValue:
                if (inDataSize != sizeof(UInt32)) { err = kAudioHardwareBadPropertySizeError; break; }
                if (isOut) gOutputMute = *(const UInt32 *)inData != 0; else gInputMute = *(const UInt32 *)inData != 0;
                break;
            default: err = kAudioHardwareUnknownPropertyError;
        }
        if (!err) apply_gains();
        pthread_mutex_unlock(&gStateMutex);
        if (!err) { save_config(); notify_controls(); }
        return err;
    }
    if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        /* Only the switch-selected rate exists; accept it, refuse anything else. */
        return (*(const Float64 *)inData == gSampleRate) ? 0 : kAudioHardwareIllegalOperationError;
    }
    if (is_stream(inObjectID)) {
        switch (inAddress->mSelector) {
            case kAudioStreamPropertyIsActive:
                if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                if (inObjectID == kObjectID_Stream_Input) gInputStreamActive = *(const UInt32 *)inData != 0; else gOutputStreamActive = *(const UInt32 *)inData != 0;
                return 0;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                const AudioStreamBasicDescription *d = (const AudioStreamBasicDescription *)inData;
                if (d->mFormatID != kAudioFormatLinearPCM || d->mChannelsPerFrame != kChannels || d->mSampleRate != gSampleRate || !(d->mFormatFlags & kAudioFormatFlagIsFloat) || d->mBitsPerChannel != 32)
                    return kAudioDeviceUnsupportedFormatError;
                return 0;
            }
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ------------------------------------------------------------------------- */
/* IO                                                                          */

static OSStatus UA4FX_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    OSStatus err = 0;
    pthread_mutex_lock(&gStateMutex);
    if (!atomic_load(&gDevicePresent)) err = kAudioHardwareNotRunningError;
    else if (gIOCount == 0) {
        int r = ua4fx_engine_start(gEngine);
        if (r) { LOGE("HAL: engine start failed 0x%x", r); err = kAudioHardwareUnspecifiedError; }
        else gIOCount = 1;
    } else if (gIOCount == UINT32_MAX) err = kAudioHardwareIllegalOperationError;
    else gIOCount++;
    pthread_mutex_unlock(&gStateMutex);
    return err;
}

static OSStatus UA4FX_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gIOCount == 1) { gIOCount = 0; ua4fx_engine_stop(gEngine); }
    else if (gIOCount > 1) gIOCount--;
    pthread_mutex_unlock(&gStateMutex);
    return 0;
}

static OSStatus UA4FX_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    double st; uint64_t ht, seed;
    if (!ua4fx_engine_get_zero_timestamp(gEngine, &st, &ht, &seed)) return kAudioHardwareNotRunningError;
    *outSampleTime = st; *outHostTime = ht; *outSeed = seed;
    return 0;
}

static OSStatus UA4FX_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace) {
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    Boolean willDo = false;
    switch (inOperationID) {
        case kAudioServerPlugInIOOperationReadInput: willDo = true; break;
        case kAudioServerPlugInIOOperationWriteMix:  willDo = true; break;
    }
    if (outWillDo) *outWillDo = willDo;
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return 0;
}

static OSStatus UA4FX_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

static OSStatus UA4FX_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer) {
    (void)inClientID; (void)ioSecondaryBuffer;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (!ioMainBuffer || !inIOCycleInfo) return kAudioHardwareIllegalOperationError;
    if (inIOBufferFrameSize > UA4FX_RING_FRAMES / 4) return kAudioHardwareIllegalOperationError;

    /* scratch buffer for the device's native 24-bit packed frames. The HAL runs
     * all IO operations of a device on one IO thread, sequentially. */
    static uint8_t native[UA4FX_RING_FRAMES / 4 * UA4FX_BYTES_PER_FRAME];
    const UInt32 nSamples = inIOBufferFrameSize * kChannels;

    if (inOperationID == kAudioServerPlugInIOOperationReadInput && inStreamObjectID == kObjectID_Stream_Input) {
        int64_t t = (int64_t)inIOCycleInfo->mInputTime.mSampleTime;
        ua4fx_engine_read_input(gEngine, t, inIOBufferFrameSize, native);
        Float32 *out = (Float32 *)ioMainBuffer;
        const float scale = atomic_load(&gInputGain) / 8388608.0f;
        for (UInt32 i = 0; i < nSamples; i++) {
            const uint8_t *p = native + i * 3;
            int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8;   /* sign-extend 24 → 32 */
            out[i] = (Float32)v * scale;
        }
        return 0;
    }
    if (inOperationID == kAudioServerPlugInIOOperationWriteMix && inStreamObjectID == kObjectID_Stream_Output) {
        const Float32 *in = (const Float32 *)ioMainBuffer;
        const float g = atomic_load(&gOutputGain);
        for (UInt32 i = 0; i < nSamples; i++) {
            float f = in[i] * g;
            if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
            int32_t v = (int32_t)(f * 8388607.0f);
            uint8_t *p = native + i * 3; p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
        }
        int64_t t = (int64_t)inIOCycleInfo->mOutputTime.mSampleTime;
        ua4fx_engine_write_output(gEngine, t, inIOBufferFrameSize, native);
        return 0;
    }
    return 0;
}

static OSStatus UA4FX_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

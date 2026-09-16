/*
 * hal_harness — loads the built UA4FX.driver bundle the way coreaudiod would
 * (factory → AudioServerPlugInDriverInterface), provides a fake host, walks
 * the property tree and runs an IO loop calling DoIOOperation exactly like the
 * HAL's IO thread. Lets us validate the plug-in without installing it.
 *
 *   hal_harness build/UA4FX.driver [seconds]
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <mach/mach_time.h>

static OSStatus host_PropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a) {
    (void)h; for (UInt32 i = 0; i < n; i++) printf("  [host] PropertiesChanged obj=%u sel='%c%c%c%c'\n", obj, (a[i].mSelector>>24)&255,(a[i].mSelector>>16)&255,(a[i].mSelector>>8)&255,a[i].mSelector&255); return 0;
}
static OSStatus host_CopyFromStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *out) { (void)h;(void)k; *out = NULL; return kAudioHardwareUnknownPropertyError; }
static OSStatus host_WriteToStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) { (void)h;(void)k;(void)d; return 0; }
static OSStatus host_DeleteFromStorage(AudioServerPlugInHostRef h, CFStringRef k) { (void)h;(void)k; return 0; }
static OSStatus host_RequestDeviceConfigurationChange(AudioServerPlugInHostRef h, AudioObjectID d, UInt64 act, void *info) { (void)h;(void)d;(void)act;(void)info; return 0; }
static AudioServerPlugInHostInterface gHostIface = { host_PropertiesChanged, host_CopyFromStorage, host_WriteToStorage, host_DeleteFromStorage, host_RequestDeviceConfigurationChange };

static double tps(void) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return 1e9 * tb.denom / tb.numer; }

#define CHECK(x) do { OSStatus _e = (x); if (_e) { printf("FAIL %s -> %d ('%c%c%c%c')\n", #x, (int)_e, (_e>>24)&255,(_e>>16)&255,(_e>>8)&255,_e&255); fails++; } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s path/to/UA4FX.driver [seconds]\n", argv[0]); return 2; }
    int seconds = argc > 2 ? atoi(argv[2]) : 4; int fails = 0;
    char exe[1024]; snprintf(exe, sizeof exe, "%s/Contents/MacOS/UA4FX", argv[1]);
    void *h = dlopen(exe, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    void *(*factory)(CFAllocatorRef, CFUUIDRef) = dlsym(h, "UA4FX_Create");
    if (!factory) { fprintf(stderr, "no factory symbol\n"); return 1; }
    AudioServerPlugInDriverRef drv = factory(NULL, kAudioServerPlugInTypeUUID);
    if (!drv) { fprintf(stderr, "factory returned NULL\n"); return 1; }
    printf("factory OK\n");

    CHECK((*drv)->Initialize(drv, (AudioServerPlugInHostRef)&gHostIface));
    usleep(300000);

    AudioObjectPropertyAddress addr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0; AudioObjectID devs[4] = {0};
    CHECK((*drv)->GetPropertyDataSize(drv, kAudioObjectPlugInObject, 0, &addr, 0, NULL, &size));
    CHECK((*drv)->GetPropertyData(drv, kAudioObjectPlugInObject, 0, &addr, 0, NULL, sizeof devs, &size, devs));
    printf("device list: %u device(s)\n", size / (UInt32)sizeof(AudioObjectID));
    if (size == 0) { printf("no device present — is the UA-4FX connected in Advanced mode?\n"); return 1; }
    AudioObjectID dev = devs[0];

    CFStringRef s = NULL;
    addr.mSelector = kAudioObjectPropertyName; CHECK((*drv)->GetPropertyData(drv, dev, 0, &addr, 0, NULL, sizeof s, &size, &s)); char buf[128]; CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8); printf("name: %s\n", buf);
    addr.mSelector = kAudioDevicePropertyDeviceUID; CHECK((*drv)->GetPropertyData(drv, dev, 0, &addr, 0, NULL, sizeof s, &size, &s)); CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8); printf("uid: %s\n", buf);
    /* translate UID back */
    addr.mSelector = kAudioPlugInPropertyTranslateUIDToDevice; AudioObjectID back = 0; CHECK((*drv)->GetPropertyData(drv, kAudioObjectPlugInObject, 0, &addr, sizeof s, &s, sizeof back, &size, &back)); printf("uid->device: %u (expect %u)\n", back, dev); if (back != dev) fails++;
    Float64 rate = 0; addr.mSelector = kAudioDevicePropertyNominalSampleRate; CHECK((*drv)->GetPropertyData(drv, dev, 0, &addr, 0, NULL, sizeof rate, &size, &rate)); printf("rate: %.0f\n", rate);
    UInt32 u; addr.mSelector = kAudioDevicePropertySafetyOffset; addr.mScope = kAudioObjectPropertyScopeInput; CHECK((*drv)->GetPropertyData(drv, dev, 0, &addr, 0, NULL, sizeof u, &size, &u)); UInt32 safIn = u;
    addr.mScope = kAudioObjectPropertyScopeOutput; CHECK((*drv)->GetPropertyData(drv, dev, 0, &addr, 0, NULL, sizeof u, &size, &u)); UInt32 safOut = u;
    addr.mSelector = kAudioDevicePropertyZeroTimeStampPeriod; addr.mScope = kAudioObjectPropertyScopeGlobal; CHECK((*drv)->GetPropertyData(drv, dev, 0, &addr, 0, NULL, sizeof u, &size, &u));
    printf("safety in=%u out=%u zts period=%u\n", safIn, safOut, u);
    AudioObjectID streams[4]; addr.mSelector = kAudioDevicePropertyStreams; CHECK((*drv)->GetPropertyData(drv, dev, 0, &addr, 0, NULL, sizeof streams, &size, streams)); UInt32 ns = size / sizeof(AudioObjectID); printf("streams: %u\n", ns);
    AudioObjectID inStream = 0, outStream = 0;
    for (UInt32 i = 0; i < ns; i++) {
        UInt32 dir; AudioObjectPropertyAddress sa = { kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CHECK((*drv)->GetPropertyData(drv, streams[i], 0, &sa, 0, NULL, sizeof dir, &size, &dir));
        AudioStreamBasicDescription f; sa.mSelector = kAudioStreamPropertyPhysicalFormat; CHECK((*drv)->GetPropertyData(drv, streams[i], 0, &sa, 0, NULL, sizeof f, &size, &f));
        printf("  stream %u: %s, %.0f Hz, %u ch, %u bits, flags 0x%x\n", streams[i], dir ? "input" : "output", f.mSampleRate, f.mChannelsPerFrame, f.mBitsPerChannel, f.mFormatFlags);
        if (dir) inStream = streams[i]; else outStream = streams[i];
    }
    /* every property HasProperty claims must be gettable: probe the selectors the HAL uses */
    {
        static const AudioObjectPropertySelector devSels[] = {
            kAudioObjectPropertyBaseClass, kAudioObjectPropertyClass, kAudioObjectPropertyOwner, kAudioObjectPropertyName, kAudioObjectPropertyManufacturer,
            kAudioObjectPropertyOwnedObjects, kAudioDevicePropertyDeviceUID, kAudioDevicePropertyModelUID, kAudioDevicePropertyTransportType,
            kAudioDevicePropertyRelatedDevices, kAudioDevicePropertyClockDomain, kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyDeviceIsRunning,
            kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioDevicePropertyLatency,
            kAudioDevicePropertyStreams, kAudioObjectPropertyControlList, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyNominalSampleRate,
            kAudioDevicePropertyAvailableNominalSampleRates, kAudioDevicePropertyIsHidden, kAudioDevicePropertyZeroTimeStampPeriod,
            kAudioDevicePropertyClockAlgorithm, kAudioDevicePropertyClockIsStable, kAudioDevicePropertyPreferredChannelsForStereo, kAudioDevicePropertyPreferredChannelLayout };
        static const AudioObjectPropertySelector strSels[] = {
            kAudioObjectPropertyBaseClass, kAudioObjectPropertyClass, kAudioObjectPropertyOwner, kAudioObjectPropertyName, kAudioObjectPropertyOwnedObjects,
            kAudioStreamPropertyIsActive, kAudioStreamPropertyDirection, kAudioStreamPropertyTerminalType, kAudioStreamPropertyStartingChannel,
            kAudioStreamPropertyLatency, kAudioStreamPropertyVirtualFormat, kAudioStreamPropertyAvailableVirtualFormats, kAudioStreamPropertyPhysicalFormat,
            kAudioStreamPropertyAvailablePhysicalFormats };
        static const AudioObjectPropertyScope scopes[] = { kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeOutput };
        int probed = 0;
        for (int sc = 0; sc < 3; sc++) {
            for (size_t i = 0; i < sizeof devSels / sizeof devSels[0]; i++) {
                AudioObjectPropertyAddress pa = { devSels[i], scopes[sc], kAudioObjectPropertyElementMain };
                if (!(*drv)->HasProperty(drv, dev, 0, &pa)) continue;
                UInt32 sz = 0; uint8_t tmp[512]; Boolean settable;
                OSStatus e1 = (*drv)->GetPropertyDataSize(drv, dev, 0, &pa, 0, NULL, &sz);
                OSStatus e2 = sz ? (*drv)->GetPropertyData(drv, dev, 0, &pa, 0, NULL, sizeof tmp, &sz, tmp) : 0;
                OSStatus e3 = (*drv)->IsPropertySettable(drv, dev, 0, &pa, &settable);
                if (e1 || e2 || e3) { printf("FAIL device prop '%c%c%c%c' scope %d: %d/%d/%d\n", (devSels[i]>>24)&255,(devSels[i]>>16)&255,(devSels[i]>>8)&255,devSels[i]&255, sc, (int)e1,(int)e2,(int)e3); fails++; }
                probed++;
            }
            for (int st = 0; st < 2; st++) for (size_t i = 0; i < sizeof strSels / sizeof strSels[0]; i++) {
                AudioObjectID sid = st ? outStream : inStream;
                AudioObjectPropertyAddress pa = { strSels[i], scopes[sc], kAudioObjectPropertyElementMain };
                if (!(*drv)->HasProperty(drv, sid, 0, &pa)) continue;
                UInt32 sz = 0; uint8_t tmp[512]; Boolean settable;
                OSStatus e1 = (*drv)->GetPropertyDataSize(drv, sid, 0, &pa, 0, NULL, &sz);
                OSStatus e2 = sz ? (*drv)->GetPropertyData(drv, sid, 0, &pa, 0, NULL, sizeof tmp, &sz, tmp) : 0;
                OSStatus e3 = (*drv)->IsPropertySettable(drv, sid, 0, &pa, &settable);
                if (e1 || e2 || e3) { printf("FAIL stream prop '%c%c%c%c' scope %d: %d/%d/%d\n", (strSels[i]>>24)&255,(strSels[i]>>16)&255,(strSels[i]>>8)&255,strSels[i]&255, sc, (int)e1,(int)e2,(int)e3); fails++; }
                probed++;
            }
        }
        printf("probed %d property/scope combinations\n", probed);
    }

    Boolean will, inPlace;
    CHECK((*drv)->WillDoIOOperation(drv, dev, 1, kAudioServerPlugInIOOperationReadInput, &will, &inPlace)); printf("ReadInput: %d\n", will);
    CHECK((*drv)->WillDoIOOperation(drv, dev, 1, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace)); printf("WriteMix: %d\n", will);

    CHECK((*drv)->StartIO(drv, dev, 1));
    addr.mSelector = kAudioDevicePropertyDeviceIsRunning; CHECK((*drv)->GetPropertyData(drv, dev, 0, &addr, 0, NULL, sizeof u, &size, &u)); printf("running: %u\n", u);

    const UInt32 bufFrames = 512; Float32 *io = calloc(bufFrames * 2, sizeof(Float32));
    double phase = 0; double T = tps(); uint64_t t0 = mach_absolute_time();
    Float64 lastZ = -1; UInt64 lastH = 0; int nZ = 0; double maxErrUs = 0; double peak = 0; UInt64 cycles = 0;
    int64_t nextOut = -1, nextIn = -1;
    AudioServerPlugInIOCycleInfo ci; memset(&ci, 0, sizeof ci);
    while ((double)(mach_absolute_time() - t0) / T < seconds) {
        Float64 zs; UInt64 zh, seed;
        CHECK((*drv)->GetZeroTimeStamp(drv, dev, 1, &zs, &zh, &seed));
        if (zs != lastZ) { if (lastZ >= 0) { double dt = (double)(zh - lastH) / T; double err = fabs(dt - (zs - lastZ) / rate) * 1e6; if (err > maxErrUs) maxErrUs = err; } lastZ = zs; lastH = zh; nZ++; }
        uint64_t now = mach_absolute_time(); double nowS = zs + ((double)now - (double)zh) / T * rate;
        if (nextOut < 0) { nextOut = (int64_t)nowS + safOut + bufFrames; nextIn = (int64_t)nowS - safIn - 2 * bufFrames; }
        ci.mIOCycleCounter = ++cycles; ci.mNominalIOBufferFrameSize = bufFrames;
        while ((double)nextOut < nowS + safOut + 2 * bufFrames) {
            for (UInt32 k = 0; k < bufFrames; k++) { float v = 0.1f * sinf((float)phase); phase += 2 * M_PI * 440.0 / rate; io[2*k] = io[2*k+1] = v; }
            ci.mOutputTime.mSampleTime = (Float64)nextOut; ci.mOutputTime.mFlags = kAudioTimeStampSampleTimeValid;
            CHECK((*drv)->BeginIOOperation(drv, dev, 1, kAudioServerPlugInIOOperationWriteMix, bufFrames, &ci));
            CHECK((*drv)->DoIOOperation(drv, dev, outStream, 1, kAudioServerPlugInIOOperationWriteMix, bufFrames, &ci, io, NULL));
            CHECK((*drv)->EndIOOperation(drv, dev, 1, kAudioServerPlugInIOOperationWriteMix, bufFrames, &ci));
            nextOut += bufFrames;
        }
        while ((double)nextIn + bufFrames < nowS - safIn) {
            ci.mInputTime.mSampleTime = (Float64)nextIn; ci.mInputTime.mFlags = kAudioTimeStampSampleTimeValid;
            CHECK((*drv)->DoIOOperation(drv, dev, inStream, 1, kAudioServerPlugInIOOperationReadInput, bufFrames, &ci, io, NULL));
            for (UInt32 k = 0; k < bufFrames * 2; k++) { double a = fabs(io[k]); if (a > peak) peak = a; }
            nextIn += bufFrames;
        }
        usleep(2000);
    }
    printf("IO loop: %llu cycles, %d zero timestamps, max ZTS interval error %.1f us, input peak %.4f (%.1f dBFS)\n", (unsigned long long)cycles, nZ, maxErrUs, peak, peak > 0 ? 20*log10(peak) : -200.0);
    CHECK((*drv)->StopIO(drv, dev, 1));
    addr.mSelector = kAudioDevicePropertyDeviceIsRunning; CHECK((*drv)->GetPropertyData(drv, dev, 0, &addr, 0, NULL, sizeof u, &size, &u)); printf("running after stop: %u\n", u);
    /* second start/stop cycle to test restart */
    CHECK((*drv)->StartIO(drv, dev, 2)); usleep(200000); CHECK((*drv)->StopIO(drv, dev, 2));
    printf("%s (%d failures)\n", fails ? "HARNESS FAILED" : "HARNESS PASSED", fails);
    return fails ? 1 : 0;
}

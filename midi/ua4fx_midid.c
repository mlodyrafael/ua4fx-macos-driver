/*
 * ua4fx_midid — bridges the UA-4FX's MIDI interface (USB-MIDI 1.0 packets on a
 * vendor-specific interface, class 0xFF / subclass 0x03) to CoreMIDI as a
 * virtual source "UA-4FX MIDI In" and a virtual destination "UA-4FX MIDI Out".
 *
 * Runs as a per-user launch agent; needs no privileges. Uses only the MIDI
 * interface, so it coexists with the HAL plug-in that owns the audio interfaces.
 * Like Linux (snd_usbmidi_switch_roland_altsetting) it selects altsetting 1,
 * where the IN endpoint is interrupt-type, when that altsetting exists.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOMessage.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../driver/UA4FX_USB.h"
#include "../driver/UA4FX_Log.h"

#define RXBUF 64

static MIDIClientRef gClient; static MIDIEndpointRef gSource, gDest;
static IONotificationPortRef gNotify; static io_iterator_t gIfIter, gDevIter;
static IOUSBInterfaceInterface942 **gIf; static io_object_t gInterest; static CFRunLoopSourceRef gSrc;
static UInt8 gInPipe, gOutPipe; static uint8_t gRx[RXBUF];
static uint8_t gCin[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };

/* ---------- USB → CoreMIDI ---------- */
static void rx_complete(void *refcon, IOReturn result, void *arg0);
static void submit_read(void) {
    if (!gIf) return;
    IOReturn kr = (*gIf)->ReadPipeAsync(gIf, gInPipe, gRx, RXBUF, rx_complete, NULL);
    if (kr) LOGE("ReadPipeAsync failed 0x%x", kr);
}
static void rx_complete(void *refcon, IOReturn result, void *arg0) {
    (void)refcon;
    if (result == kIOReturnAborted || result == kIOReturnNoDevice || result == kIOReturnNotResponding || !gIf) return;
    if (result == kIOReturnSuccess) {
        size_t n = (size_t)(uintptr_t)arg0;
        Byte plbuf[1024]; MIDIPacketList *pl = (MIDIPacketList *)plbuf; MIDIPacket *pk = MIDIPacketListInit(pl);
        MIDITimeStamp now = 0; /* 0 == now */
        for (size_t i = 0; i + 4 <= n; i += 4) {
            uint8_t cin = gRx[i] & 0x0F; uint8_t len = gCin[cin];
            if (!len) continue;
            pk = MIDIPacketListAdd(pl, sizeof plbuf, pk, now, len, &gRx[i + 1]);
            if (!pk) break;
        }
        if (pl->numPackets) MIDIReceived(gSource, pl);
    }
    submit_read();
}

/* ---------- CoreMIDI → USB ---------- */
static uint8_t gRunning = 0; static int gInSysex = 0; static uint8_t gSyx[3]; static int gSyxN = 0;
static void usb_send(const uint8_t *pkts, size_t n) {
    if (!gIf || !n) return;
    IOReturn kr = (*gIf)->WritePipe(gIf, gOutPipe, (void *)pkts, (UInt32)n);
    if (kr) LOGE("WritePipe failed 0x%x", kr);
}
static void encode_bytes(const uint8_t *b, size_t n) {
    uint8_t out[512]; size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xF8) { out[o++] = 0x0F; out[o++] = c; out[o++] = 0; out[o++] = 0; }         /* realtime */
        else if (gInSysex) {
            gSyx[gSyxN++] = c;
            if (c == 0xF7) { out[o++] = (uint8_t)(0x04 + gSyxN); memcpy(&out[o], gSyx, 3); o += 3; gSyxN = 0; gInSysex = 0; memset(gSyx, 0, 3); }
            else if (gSyxN == 3) { out[o++] = 0x04; memcpy(&out[o], gSyx, 3); o += 3; gSyxN = 0; memset(gSyx, 0, 3); }
        }
        else if (c == 0xF0) { gInSysex = 1; gSyxN = 0; memset(gSyx, 0, 3); gSyx[gSyxN++] = c; }
        else if (c >= 0x80) {                                                                   /* status */
            uint8_t hi = c >> 4; size_t need = (hi == 0xC || hi == 0xD) ? 1 : 2;
            if (c == 0xF1 || c == 0xF3) need = 1; else if (c == 0xF2) need = 2; else if (c == 0xF6 || c == 0xF4 || c == 0xF5) need = 0;
            if (i + need >= n) break;
            uint8_t cin = c < 0xF0 ? hi : (need == 0 ? 0x5 : need == 1 ? 0x2 : 0x3);
            out[o++] = cin; out[o++] = c; out[o++] = need >= 1 ? b[i + 1] : 0; out[o++] = need >= 2 ? b[i + 2] : 0;
            if (c < 0xF0) gRunning = c;
            i += need;
        } else if (gRunning) {                                                                  /* running status */
            uint8_t hi = gRunning >> 4; size_t need = (hi == 0xC || hi == 0xD) ? 1 : 2;
            if (i + need > n) break;
            out[o++] = hi; out[o++] = gRunning; out[o++] = b[i]; out[o++] = need == 2 ? b[i + 1] : 0;
            i += need - 1;
        }
        if (o + 8 > sizeof out) { usb_send(out, o); o = 0; }
    }
    usb_send(out, o);
}
static void dest_read(const MIDIPacketList *pl, void *rc, void *src) {
    (void)rc; (void)src;
    const MIDIPacket *p = &pl->packet[0];
    for (UInt32 i = 0; i < pl->numPackets; i++) { encode_bytes(p->data, p->length); p = MIDIPacketNext(p); }
}

/* ---------- interface attach / detach ---------- */
static void set_online(Boolean online) {
    MIDIObjectSetIntegerProperty(gSource, kMIDIPropertyOffline, !online);
    MIDIObjectSetIntegerProperty(gDest, kMIDIPropertyOffline, !online);
}
static void detach(void) {
    if (!gIf) return;
    LOG("MIDI interface detached");
    (*gIf)->AbortPipe(gIf, gInPipe);
    if (gSrc) { CFRunLoopRemoveSource(CFRunLoopGetCurrent(), gSrc, kCFRunLoopDefaultMode); gSrc = NULL; }
    if (gInterest) { IOObjectRelease(gInterest); gInterest = 0; }
    (*gIf)->USBInterfaceClose(gIf); (*gIf)->Release(gIf); gIf = NULL;
    set_online(false);
}
static void interest(void *rc, io_service_t s, natural_t msg, void *arg) { (void)rc;(void)s;(void)arg; if (msg == kIOMessageServiceIsTerminated) detach(); }

static void attach(io_service_t ifs) {
    if (gIf) return;
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    if (IOCreatePlugInInterfaceForService(ifs, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score) || !plug) return;
    IOUSBInterfaceInterface942 **ifc = NULL; (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID942), (LPVOID *)&ifc); (*plug)->Release(plug);
    if (!ifc) return;
    if ((*ifc)->USBInterfaceOpen(ifc)) { LOGE("USBInterfaceOpen failed"); (*ifc)->Release(ifc); return; }
    /* prefer altsetting 1 (interrupt IN) like Linux; fall back to alt 0 */
    if ((*ifc)->SetAlternateInterface(ifc, 1)) (*ifc)->SetAlternateInterface(ifc, 0);
    UInt8 neps = 0; (*ifc)->GetNumEndpoints(ifc, &neps); gInPipe = gOutPipe = 0;
    for (UInt8 p = 1; p <= neps; p++) { UInt8 dir, num, tt, ivl; UInt16 mps; if ((*ifc)->GetPipeProperties(ifc, p, &dir, &num, &tt, &mps, &ivl)) continue;
        if (tt != kUSBBulk && tt != kUSBInterrupt) continue; if (dir == kUSBIn && !gInPipe) gInPipe = p; if (dir == kUSBOut && !gOutPipe) gOutPipe = p; }
    if (!gInPipe || !gOutPipe) { LOGE("MIDI pipes not found"); (*ifc)->USBInterfaceClose(ifc); (*ifc)->Release(ifc); return; }
    if ((*ifc)->CreateInterfaceAsyncEventSource(ifc, &gSrc) == 0) CFRunLoopAddSource(CFRunLoopGetCurrent(), gSrc, kCFRunLoopDefaultMode);
    IOServiceAddInterestNotification(gNotify, ifs, kIOGeneralInterest, interest, NULL, &gInterest);
    gIf = ifc; gRunning = 0; gInSysex = 0;
    set_online(true);
    submit_read();
    LOG("MIDI interface attached (in pipe %u, out pipe %u)", gInPipe, gOutPipe);
}
static void ifaces_added(void *rc, io_iterator_t it) { (void)rc; io_service_t s; while ((s = IOIteratorNext(it))) { attach(s); IOObjectRelease(s); } }

/* If nothing has configured the device yet (HAL plug-in absent), do it so the
 * interfaces appear. Harmless if the device is already configured or busy. */
static void devices_added(void *rc, io_iterator_t it) {
    (void)rc; io_service_t s;
    while ((s = IOIteratorNext(it))) {
        IOCFPlugInInterface **plug = NULL; SInt32 score;
        if (IOCreatePlugInInterfaceForService(s, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score) == 0 && plug) {
            IOUSBDeviceInterface942 **dev = NULL; (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942), (LPVOID *)&dev); (*plug)->Release(plug);
            if (dev) { UInt8 c = 0; (*dev)->GetConfiguration(dev, &c);
                if (c == 0 && (*dev)->USBDeviceOpen(dev) == 0) { (*dev)->SetConfiguration(dev, 1); (*dev)->USBDeviceClose(dev); }
                (*dev)->Release(dev); }
        }
        IOObjectRelease(s);
    }
}

static void midi_notify(const MIDINotification *m, void *rc) { (void)m; (void)rc; }

int main(void) {
    OSStatus st = MIDIClientCreate(CFSTR("UA-4FX Bridge"), midi_notify, NULL, &gClient);
    if (st) { LOGE("MIDIClientCreate failed %d", (int)st); return 1; }
    MIDISourceCreate(gClient, CFSTR("UA-4FX MIDI In"), &gSource);
    MIDIDestinationCreate(gClient, CFSTR("UA-4FX MIDI Out"), dest_read, NULL, &gDest);
    MIDIObjectSetIntegerProperty(gSource, kMIDIPropertyUniqueID, 0x5A4F0001);
    MIDIObjectSetIntegerProperty(gDest, kMIDIPropertyUniqueID, 0x5A4F0002);
    MIDIObjectSetStringProperty(gSource, kMIDIPropertyManufacturer, CFSTR("Roland / EDIROL"));
    MIDIObjectSetStringProperty(gDest, kMIDIPropertyManufacturer, CFSTR("Roland / EDIROL"));
    MIDIObjectSetStringProperty(gSource, kMIDIPropertyModel, CFSTR("UA-4FX"));
    MIDIObjectSetStringProperty(gDest, kMIDIPropertyModel, CFSTR("UA-4FX"));
    set_online(false);

    gNotify = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(gNotify), kCFRunLoopDefaultMode);
    int vid = UA4FX_VID, pid = UA4FX_PID_ADVANCED, cls = 0xFF, sub = 0x03;
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostInterface");
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid), p = CFNumberCreate(NULL, kCFNumberIntType, &pid), c = CFNumberCreate(NULL, kCFNumberIntType, &cls), sc = CFNumberCreate(NULL, kCFNumberIntType, &sub);
    CFDictionarySetValue(m, CFSTR("idVendor"), v); CFDictionarySetValue(m, CFSTR("idProduct"), p); CFDictionarySetValue(m, CFSTR("bInterfaceClass"), c); CFDictionarySetValue(m, CFSTR("bInterfaceSubClass"), sc);
    IOServiceAddMatchingNotification(gNotify, kIOFirstMatchNotification, m, ifaces_added, NULL, &gIfIter);
    ifaces_added(NULL, gIfIter);
    CFMutableDictionaryRef dm = IOServiceMatching(kIOUSBDeviceClassName);
    CFDictionarySetValue(dm, CFSTR(kUSBVendorID), v); CFDictionarySetValue(dm, CFSTR(kUSBProductID), p);
    IOServiceAddMatchingNotification(gNotify, kIOFirstMatchNotification, dm, devices_added, NULL, &gDevIter);
    devices_added(NULL, gDevIter);
    CFRelease(v); CFRelease(p); CFRelease(c); CFRelease(sc);

    LOG("ua4fx_midid running");
    CFRunLoopRun();
    return 0;
}

/*
 * UA4FX_USB.c — user-space isochronous streaming engine for the EDIROL UA-4FX.
 *
 * Design
 * ------
 * * One dedicated real-time thread ("USB thread") owns every IOKit object and
 *   runs a CFRunLoop that services IOUSBLib async completions and IOKit
 *   hot-plug notifications. Other threads talk to it through engine_sync().
 * * Audio is exchanged with the CoreAudio HAL through two lock-free ring
 *   buffers indexed by absolute device sample time (single producer, single
 *   consumer each).
 * * The device clock: while the capture stream runs, the number of frames the
 *   ADC delivered is the device's sample clock ("capture master") and the
 *   playback packet sizes track it (implicit feedback — the OUT endpoint is
 *   adaptive). Without capture (96 kHz PLAY mode) the nominal rate on the USB
 *   SOF clock is used instead.
 * * Zero timestamps (sample time, host time) are derived from the per-frame
 *   completion timestamps the host controller records (frTimeStamp), falling
 *   back to extrapolation from GetBusFrameNumberWithTime().
 *
 * The device needs no vendor requests: SET_CONFIGURATION(1), then
 * SET_INTERFACE(alt 1) on the PCM interfaces, then isochronous traffic.
 * (Matches what Linux snd-usb-audio's create_uaxx_quirk() does.)
 */
#include "UA4FX_USB.h"
#include "UA4FX_Log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOMessage.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define NF   ((int)e->nf)      /* runtime geometry; `e` must be in scope */
#define NBUF ((int)e->nbuf)
#define RING UA4FX_RING_FRAMES
#define BPF  UA4FX_BYTES_PER_FRAME
#define START_LEAD_FRAMES 4     /* schedule the first transfer this many bus frames ahead */

typedef struct ua4fx_engine engine_t;

typedef struct xfer {
    IOUSBLowLatencyIsocFrame *fl;
    uint8_t  *buf;
    UInt64    frame;       /* first bus frame number of this transfer */
    uint64_t  txEnd;       /* playback: ring position after this transfer was filled */
    int       idx;
    bool      inFlight;
    struct stream *stream;
} xfer_t;

typedef struct stream {
    engine_t *e;
    IOUSBInterfaceInterface942 **ifc;
    CFRunLoopSourceRef src;
    UInt8   ifnum, pipe, epAddr;
    UInt16  maxPacket;
    bool    isInput;
    bool    valid;        /* interface found and opened */
    bool    streaming;    /* alt 1 selected and transfers queued */
    bool    stopping;
    xfer_t  xf[UA4FX_MAX_XFERS_IN_FLIGHT];
    int     inFlight;
    UInt64  nextFrame;    /* next bus frame to schedule */
    UInt64  lastFrameDone;/* last bus frame whose completion we processed */
} stream_t;

struct ua4fx_engine {
    /* thread */
    pthread_t   thread;
    CFRunLoopRef rl;
    dispatch_semaphore_t ready;
    atomic_bool quit;

    /* hot-plug */
    IONotificationPortRef notifyPort;
    io_iterator_t addIter, removeIter;
    ua4fx_hotplug_cb hotplugCb;
    void *hotplugCtx;

    /* device */
    atomic_bool present;
    io_service_t devService;
    io_object_t  interestNotification;
    IOUSBDeviceInterface942 **dev;
    _Atomic uint32_t rate;
    uint32_t locationID;
    char     serial[64];
    UInt8    midiIfnum;   /* 0xFF if none */
    stream_t in, out;

    /* geometry (set before start) */
    uint32_t nf, nbuf;

    /* IO state */
    atomic_bool running;
    int startRefs;
    uint8_t *inRing;
    uint8_t *outRing;

    /* counters (USB thread only unless atomic) */
    _Atomic uint64_t devSampleTime;   /* device clock, frames since start */
    uint64_t rxCompleted;             /* capture frames received */
    uint64_t txCompleted;             /* playback frames confirmed sent */
    uint64_t txSubmitted;             /* playback frames handed to the HC (ring read pos) */
    uint64_t rxPackets, txPackets, rxErrors, txErrors, resyncs;
    double   nominalPerMs;            /* 44.1 / 48 / 96 */
    double   outAccum;                /* fractional packet-size accumulator */
    double   outRate;                 /* playback frames per bus frame in use */
    uint64_t rxBusFrames;             /* bus frames accounted on the capture side */
    uint32_t packetsAdjusted;
    int32_t  feedbackErr;
    bool     captureMaster;
    uint32_t snaps;                   /* hard realignments of the playback read pointer */
    uint64_t lastResyncHost;
    double   maxCompletionLatUs;      /* decaying max of completion latency */
    uint32_t lateCompletions;         /* completions later than 1 ms after the frame ended */
    bool     rtPolicyOK;

    /* timing */
    double   ticksPerMs;              /* mach ticks per USB frame (1 ms) */
    double   ticksPerFrame;           /* mach ticks per audio frame */
    UInt64   anchorFrame;             /* bus frame number ... */
    uint64_t anchorHost;              /* ... and host time of its start */
    uint64_t lastAnchorHost;
    uint32_t ztsPeriod;
    uint64_t nextZts;
    _Atomic uint32_t ztsSeq;
    double   ztsSample;
    uint64_t ztsHost;
    uint64_t seed;
    uint64_t ioStartHost;
    int      stampPending;            /* timing-stream completions still to wait for before ZTS(0) is final */

    /* rate measurement */
    uint64_t rateT0Host; uint64_t rateN0;
    double   measuredRate;
};

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */

static inline uint64_t at2u64(AbsoluteTime t) { uint64_t v; memcpy(&v, &t, sizeof v); return v; }

static double mach_ticks_per_ms(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    return 1e6 * (double)tb.denom / (double)tb.numer;
}

static bool set_realtime(void) {
    double tpm = mach_ticks_per_ms();
    thread_time_constraint_policy_data_t p;
    p.period      = (uint32_t)(tpm * 1.0);
    p.computation = (uint32_t)(tpm * 0.15);
    p.constraint  = (uint32_t)(tpm * 0.5);
    p.preemptible = 1;
    kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                                         (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr) LOGE("thread_policy_set failed: %x", kr);
    return kr == KERN_SUCCESS;
}

/* Run fn on the USB thread and wait for it. */
typedef void (*job_fn)(engine_t *, void *);
static void engine_sync(engine_t *e, job_fn fn, void *arg) {
    if (pthread_equal(pthread_self(), e->thread)) { fn(e, arg); return; }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    CFRunLoopPerformBlock(e->rl, kCFRunLoopDefaultMode, ^{ fn(e, arg); dispatch_semaphore_signal(done); });
    CFRunLoopWakeUp(e->rl);
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);
}

/* ------------------------------------------------------------------------- */
/* descriptor parsing                                                          */

typedef struct { UInt8 ifnum; UInt8 role; UInt8 epAddr; UInt16 maxPacket; uint32_t descRate; } ifinfo_t;
enum { ROLE_NONE = 0, ROLE_PCM_OUT, ROLE_PCM_IN, ROLE_MIDI };

/* Classify interfaces the way Linux does: an interface whose altsetting 1 has
 * one isochronous endpoint is PCM (direction from the address); one with two
 * bulk/interrupt endpoints is MIDI. Returns count. */
static int parse_config(IOUSBDeviceInterface942 **dev, ifinfo_t *out, int max) {
    IOUSBConfigurationDescriptorPtr cd = NULL;
    if ((*dev)->GetConfigurationDescriptorPtr(dev, 0, &cd) || !cd) return 0;
    const uint8_t *p = (const uint8_t *)cd, *end = p + USBToHostWord(cd->wTotalLength);
    int n = 0; int cur = -1; UInt8 curAlt = 0; uint32_t curRate = 0;
    while (p + 2 <= end && p[0] >= 2) {
        UInt8 len = p[0], type = p[1];
        if (p + len > end) break;
        if (type == kUSBInterfaceDesc && len >= 9) {
            cur = -1; curAlt = p[3]; curRate = 0;
            for (int i = 0; i < n; i++) if (out[i].ifnum == p[2]) cur = i;
            if (cur < 0 && n < max) { cur = n++; memset(&out[cur], 0, sizeof out[cur]); out[cur].ifnum = p[2]; }
        } else if (type == 0x24 && len >= 11 && p[2] == 0x02 && cur >= 0) {
            /* class-specific FORMAT_TYPE: tSamFreq at offset 8 (3 bytes LE) */
            curRate = p[8] | (p[9] << 8) | (p[10] << 16);
        } else if (type == kUSBEndpointDesc && len >= 7 && cur >= 0) {
            UInt8 addr = p[2], attr = p[3] & 3; UInt16 mps = p[4] | (p[5] << 8);
            if (attr == kUSBIsoc && curAlt == 1 && out[cur].role == ROLE_NONE) {
                out[cur].role = (addr & 0x80) ? ROLE_PCM_IN : ROLE_PCM_OUT;
                out[cur].epAddr = addr; out[cur].maxPacket = mps; out[cur].descRate = curRate;
            } else if ((attr == kUSBBulk || attr == kUSBInterrupt) && curAlt == 0) {
                out[cur].role = ROLE_MIDI;
            }
        }
        p += len;
    }
    return n;
}

static uint32_t rate_from_maxpacket(UInt16 mps) {
    /* Same table as Linux create_uaxx_quirk(); tolerate small variations. */
    if (mps <= 0x120 + 6)  return 44100;   /* 0x120 */
    if (mps <= 0x140 + 6)  return 48000;   /* 0x138, 0x140 */
    if (mps >= 0x258 - 6 && mps <= 0x260 + 6) return 96000;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* zero timestamps                                                             */

static void zts_publish(engine_t *e, double sample, uint64_t host) {
    atomic_fetch_add_explicit(&e->ztsSeq, 1, memory_order_release);   /* odd = writing */
    e->ztsSample = sample; e->ztsHost = host;
    atomic_fetch_add_explicit(&e->ztsSeq, 1, memory_order_release);   /* even = stable */
}

/* Host time at the *end* of bus frame F, from the stored anchor. */
static uint64_t frame_end_host(engine_t *e, UInt64 F) {
    return e->anchorHost + (uint64_t)(((double)(F + 1 - e->anchorFrame)) * e->ticksPerMs);
}

static void refresh_anchor(engine_t *e, stream_t *s) {
    UInt64 f = 0; AbsoluteTime at;
    if ((*s->ifc)->GetBusFrameNumberWithTime(s->ifc, &f, &at) == 0) {
        uint64_t h = at2u64(at);
        if (e->anchorFrame && f > e->anchorFrame && h > e->anchorHost) {
            double tpm = (double)(h - e->anchorHost) / (double)(f - e->anchorFrame);
            /* only trust long baselines; 1 ms frames drift by ppm vs mach time */
            if (f - e->anchorFrame >= 500) e->ticksPerMs = e->ticksPerMs * 0.5 + tpm * 0.5;
        }
        if (!e->anchorFrame || f - e->anchorFrame >= 500) { e->anchorFrame = f; e->anchorHost = h; }
        e->lastAnchorHost = mach_absolute_time();
    }
}

/* Account `n` device-clock frames that ended at host time `hostEnd`. */
static void clock_advance(engine_t *e, uint32_t n, uint64_t hostEnd) {
    uint64_t before = atomic_load_explicit(&e->devSampleTime, memory_order_relaxed);
    uint64_t after  = before + n;
    while (after >= e->nextZts) {
        /* interpolate back inside this packet to the exact period boundary */
        uint64_t host = hostEnd - (uint64_t)((double)(after - e->nextZts) * e->ticksPerFrame);
        if (host <= e->ztsHost) host = e->ztsHost + 1;       /* keep monotonic */
        zts_publish(e, (double)e->nextZts, host);
        e->nextZts += e->ztsPeriod;
    }
    atomic_store_explicit(&e->devSampleTime, after, memory_order_release);

    /* slow rate measurement (diagnostics + ticksPerFrame refinement) */
    if (!e->rateT0Host) { e->rateT0Host = hostEnd; e->rateN0 = after; }
    else if (hostEnd - e->rateT0Host > (uint64_t)(e->ticksPerMs * 2000.0)) {
        double secs = (double)(hostEnd - e->rateT0Host) / (e->ticksPerMs * 1000.0);
        e->measuredRate = (double)(after - e->rateN0) / secs;
        double tpf = (double)(hostEnd - e->rateT0Host) / (double)(after - e->rateN0);
        if (tpf > 0) e->ticksPerFrame = e->ticksPerFrame * 0.75 + tpf * 0.25;
        e->rateT0Host = hostEnd; e->rateN0 = after;
    }
}

/* ------------------------------------------------------------------------- */
/* transfers                                                                   */

static void xfer_complete(void *refcon, IOReturn result, void *arg0);

/* Packet sizes come from a fractional accumulator driven by outRate (frames per
 * bus frame). outRate is the smoothed capture rate plus a slow phase correction
 * (see feedback in xfer_complete), so an adaptive DAC sees an even 48/48/…/49
 * pattern instead of jumps. */
static uint32_t next_out_packet_frames(engine_t *e) {
    e->outAccum += e->outRate;
    uint32_t n = (uint32_t)e->outAccum;
    e->outAccum -= n;
    uint32_t maxN = e->out.maxPacket / BPF;
    if (n > maxN) n = maxN;
    if (n != (uint32_t)e->nominalPerMs) e->packetsAdjusted++;
    return n;
}

static IOReturn submit_xfer(stream_t *s, xfer_t *x) {
    engine_t *e = s->e;
    x->frame = s->nextFrame;
    if (s->isInput) {
        for (int i = 0; i < NF; i++) { x->fl[i].frReqCount = s->maxPacket; x->fl[i].frActCount = 0; x->fl[i].frStatus = 0; memset(&x->fl[i].frTimeStamp, 0, sizeof(AbsoluteTime)); }
    } else {
        uint8_t *dst = x->buf;
        for (int i = 0; i < NF; i++) {
            uint32_t n = next_out_packet_frames(e);
            uint32_t bytes = n * BPF;
            /* pull from the ring at the absolute position txSubmitted and clear behind us */
            uint64_t pos = e->txSubmitted;
            for (uint32_t k = 0; k < n; k++) {
                uint8_t *src = e->outRing + ((pos + k) & (RING - 1)) * BPF;
                memcpy(dst + k * BPF, src, BPF);
                memset(src, 0, BPF);
            }
            e->txSubmitted += n;
            x->txEnd = e->txSubmitted;
            x->fl[i].frReqCount = bytes; x->fl[i].frActCount = 0; x->fl[i].frStatus = 0; memset(&x->fl[i].frTimeStamp, 0, sizeof(AbsoluteTime));
            dst += bytes;          /* frames are packed contiguously by frReqCount */
        }
    }
    IOReturn kr = s->isInput
        ? (*s->ifc)->LowLatencyReadIsochPipeAsync (s->ifc, s->pipe, x->buf, x->frame, NF, 1, x->fl, xfer_complete, x)
        : (*s->ifc)->LowLatencyWriteIsochPipeAsync(s->ifc, s->pipe, x->buf, x->frame, NF, 1, x->fl, xfer_complete, x);
    if (kr == kIOReturnSuccess) { x->inFlight = true; s->inFlight++; s->nextFrame += NF; }
    return kr;
}

/* Put the playback read pointer where the device timeline says bus frame R will
 * be: rxCompleted covers frames through in.lastFrameDone, so frame R corresponds to
 * rxCompleted + (R - lastFrameDone - 1) * nominal frames. Any output between the
 * old and the new pointer is dropped (one click) instead of chasing the phase for
 * seconds with a rate change. */
static void realign_tx(engine_t *e, UInt64 R) {
    if (!e->captureMaster || !e->out.streaming || !e->in.lastFrameDone) return;
    double ahead = (double)((int64_t)R - (int64_t)e->in.lastFrameDone - 1);
    if (ahead < 0) ahead = 0;
    uint64_t want = e->rxCompleted + (uint64_t)(ahead * e->nominalPerMs);
    e->txSubmitted = want;
    e->outAccum = 0;
    e->snaps++;
}

/* We fell behind the bus schedule on one stream: move BOTH streams to a common
 * future frame so that in/out transfers stay paired, and realign playback. */
static void engine_resync(engine_t *e, stream_t *s, const char *why) {
    UInt64 f = 0; AbsoluteTime at;
    if ((*s->ifc)->GetBusFrameNumberWithTime(s->ifc, &f, &at) != 0) return;
    UInt64 R = f + START_LEAD_FRAMES;
    if (e->in.streaming)  e->in.nextFrame  = R;
    if (e->out.streaming) e->out.nextFrame = R;
    realign_tx(e, R);
    e->resyncs++;
    uint64_t now = mach_absolute_time();
    if (now - e->lastResyncHost > (uint64_t)(e->ticksPerMs * 5000.0)) {
        e->lastResyncHost = now;
        LOGE("resync #%llu (%s on %s): bus frame %llu, rescheduling from %llu; maxCompletionLat=%.0fus late=%u nf=%u nbuf=%u",
             (unsigned long long)e->resyncs, why, s->isInput ? "in" : "out", (unsigned long long)f, (unsigned long long)R,
             e->maxCompletionLatUs, e->lateCompletions, e->nf, e->nbuf);
    }
}

/* Re-queue every idle transfer of a stream (after errors some may be parked). */
static IOReturn stream_fill_queue(stream_t *s) {
    engine_t *e = s->e; IOReturn last = 0;
    for (int b = 0; b < NBUF; b++) if (!s->xf[b].inFlight) { IOReturn kr = submit_xfer(s, &s->xf[b]); if (kr) last = kr; }
    return last;
}

static void xfer_complete(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    xfer_t *x = (xfer_t *)refcon; stream_t *s = x->stream; engine_t *e = s->e;
    x->inFlight = false; s->inFlight--;
    if (s->stopping || !atomic_load(&e->running)) return;
    if (result == kIOReturnAborted || result == kIOReturnNotResponding || result == kIOReturnNoDevice) return;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun && result != kIOReturnOverrun) {
        /* e.g. we fell behind the bus schedule: re-anchor and keep going */
        if (s->isInput) e->rxErrors += NF; else e->txErrors += NF;
        engine_resync(e, s, "completion error");
        IOReturn kr = stream_fill_queue(s);
        if (kr) LOGE("%s resubmit after error 0x%x failed: 0x%x", s->isInput ? "in" : "out", result, kr);
        return;
    }
    s->lastFrameDone = x->frame + NF - 1;
    {   /* completion latency: now vs. end of the last frame of this transfer */
        uint64_t te = at2u64(x->fl[NF - 1].frTimeStamp);
        if (te) { double lat = ((double)mach_absolute_time() - (double)te) / e->ticksPerMs * 1000.0;
                  if (lat > e->maxCompletionLatUs) e->maxCompletionLatUs = lat; else e->maxCompletionLatUs *= 0.9999;
                  if (lat > 1000.0) e->lateCompletions++; }
    }

    bool timing = s->isInput ? e->captureMaster : !e->captureMaster;
    for (int i = 0; i < NF; i++) {
        IOUSBLowLatencyIsocFrame *f = &x->fl[i];
        uint64_t hostEnd = at2u64(f->frTimeStamp);
        if (hostEnd == 0) hostEnd = frame_end_host(e, x->frame + i);
        if (s->isInput) {
            uint32_t n = f->frStatus == 0 ? f->frActCount / BPF : 0;
            if (f->frStatus && f->frStatus != kIOReturnUnderrun) e->rxErrors++;
            if (n) {
                uint64_t pos = e->rxCompleted;
                const uint8_t *src = x->buf + i * s->maxPacket;
                for (uint32_t k = 0; k < n; k++)
                    memcpy(e->inRing + ((pos + k) & (RING - 1)) * BPF, src + k * BPF, BPF);
            }
            e->rxCompleted += n; e->rxPackets++; e->rxBusFrames++;
            if (timing) clock_advance(e, n, hostEnd);
        } else {
            uint32_t n = f->frReqCount / BPF;
            if (f->frStatus) e->txErrors++;
            e->txCompleted += n; e->txPackets++;
            if (timing) clock_advance(e, n, hostEnd);
        }
    }

    /* Implicit feedback, aligned by bus frame: the playback transfer with the same
     * index covers the same bus frames (both schedules started at f0 in lock-step). */
    if (s->isInput && e->captureMaster && e->out.streaming) {
        xfer_t *tx = NULL;
        for (int b = 0; b < NBUF; b++) if (e->out.xf[b].frame == x->frame) { tx = &e->out.xf[b]; break; }
        if (tx && e->rxBusFrames >= 200) {
            int64_t err = (int64_t)tx->txEnd - (int64_t)e->rxCompleted;
            e->feedbackErr = (int32_t)err;
            double avg = (double)e->rxCompleted / (double)e->rxBusFrames;    /* device frames per bus frame */
            if (err > (int64_t)(e->nominalPerMs * 3.0) || err < -(int64_t)(e->nominalPerMs * 3.0)) {
                /* way off (after a schedule skip): hard realign instead of a long rate excursion */
                realign_tx(e, e->out.nextFrame);
                e->outRate = avg;
            } else {
                double rate = avg - (double)err / 2000.0;                      /* pull the phase error to 0 over ~2 s */
                double lo = e->nominalPerMs * 0.998, hi = e->nominalPerMs * 1.002;
                if (rate < lo) rate = lo; else if (rate > hi) rate = hi;
                e->outRate = rate;
            }
        }
    }

    /* The first isoch transfer's timestamps are not reliable on XHCI (observed 1 ms early);
     * fix the origin of the timeline from the second completed timing transfer instead. */
    if (timing && e->stampPending > 0 && --e->stampPending == 0) {
        uint64_t hostEnd = at2u64(x->fl[NF - 1].frTimeStamp);
        if (hostEnd == 0) hostEnd = frame_end_host(e, x->frame + NF - 1);
        uint64_t total = atomic_load(&e->devSampleTime);
        uint64_t host0 = hostEnd - (uint64_t)((double)total * e->ticksPerFrame);
        e->ztsHost = 0;
        zts_publish(e, 0.0, host0);
        e->ioStartHost = host0;
    }

    /* keep the anchor fresh (cheap; once a second) */
    if (timing && mach_absolute_time() - e->lastAnchorHost > (uint64_t)(e->ticksPerMs * 1000.0)) refresh_anchor(e, s);

    /* re-queue */
    IOReturn kr = submit_xfer(s, x);
    if (kr == kIOReturnIsoTooOld || kr == kIOReturnIsoTooNew) {
        engine_resync(e, s, kr == kIOReturnIsoTooOld ? "IsoTooOld" : "IsoTooNew");
        kr = stream_fill_queue(s);
    }
    if (kr) LOGE("%s resubmit failed: 0x%x", s->isInput ? "in" : "out", kr);
}

/* ------------------------------------------------------------------------- */
/* stream setup / teardown (USB thread)                                        */

static bool stream_prepare(stream_t *s) {
    engine_t *e = s->e;
    IOReturn kr = (*s->ifc)->SetAlternateInterface(s->ifc, 1);
    if (kr) { LOGE("SetAlternateInterface(if %u, alt 1) failed 0x%x (bandwidth?)", s->ifnum, kr); return false; }
    UInt8 neps = 0; (*s->ifc)->GetNumEndpoints(s->ifc, &neps);
    s->pipe = 0;
    for (UInt8 p = 1; p <= neps; p++) {
        UInt8 dir, num, tt, ivl; UInt16 mps;
        if ((*s->ifc)->GetPipeProperties(s->ifc, p, &dir, &num, &tt, &mps, &ivl) == 0 && tt == kUSBIsoc) { s->pipe = p; s->maxPacket = mps; break; }
    }
    if (!s->pipe) { LOGE("no isoc pipe on interface %u", s->ifnum); (*s->ifc)->SetAlternateInterface(s->ifc, 0); return false; }
    for (int b = 0; b < NBUF; b++) {
        xfer_t *x = &s->xf[b]; x->idx = b; x->stream = s; x->inFlight = false;
        kr = (*s->ifc)->LowLatencyCreateBuffer(s->ifc, (void **)&x->buf, s->maxPacket * NF, s->isInput ? kUSBLowLatencyReadBuffer : kUSBLowLatencyWriteBuffer);
        if (kr) { LOGE("LowLatencyCreateBuffer failed 0x%x", kr); return false; }
        kr = (*s->ifc)->LowLatencyCreateBuffer(s->ifc, (void **)&x->fl, sizeof(IOUSBLowLatencyIsocFrame) * NF, kUSBLowLatencyFrameListBuffer);
        if (kr) { LOGE("LowLatencyCreateBuffer(fl) failed 0x%x", kr); return false; }
        memset(x->buf, 0, s->maxPacket * NF);
    }
    s->stopping = false; s->inFlight = 0;
    return true;
}

static void stream_release_buffers(stream_t *s) {
    engine_t *e = s->e;
    for (int b = 0; b < NBUF; b++) {
        xfer_t *x = &s->xf[b];
        if (x->buf) { (*s->ifc)->LowLatencyDestroyBuffer(s->ifc, x->buf); x->buf = NULL; }
        if (x->fl)  { (*s->ifc)->LowLatencyDestroyBuffer(s->ifc, x->fl);  x->fl  = NULL; }
    }
}

static void stream_stop(stream_t *s) {
    if (!s->valid) return;
    s->stopping = true;
    if (s->streaming) {
        (*s->ifc)->AbortPipe(s->ifc, s->pipe);
        /* drain completions */
        for (int i = 0; i < 200 && s->inFlight > 0; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        if (s->inFlight) LOGE("%s: %d transfers still in flight after abort", s->isInput ? "in" : "out", s->inFlight);
        (*s->ifc)->SetAlternateInterface(s->ifc, 0);
        s->streaming = false;
    }
    stream_release_buffers(s);
}

/* ------------------------------------------------------------------------- */
/* start / stop                                                                */

static void job_start(engine_t *e, void *arg) {
    int *res = (int *)arg; *res = kIOReturnNoDevice;
    if (!atomic_load(&e->present)) return;
    if (atomic_load(&e->running)) { *res = 0; return; }

    memset(e->inRing, 0, RING * BPF); memset(e->outRing, 0, RING * BPF);
    e->rxCompleted = e->txCompleted = e->txSubmitted = 0;
    e->rxPackets = e->txPackets = e->rxErrors = e->txErrors = 0;
    e->outAccum = 0; e->feedbackErr = 0; e->rateT0Host = 0; e->measuredRate = 0;
    e->rxBusFrames = 0; e->packetsAdjusted = 0; e->snaps = 0; e->maxCompletionLatUs = 0; e->lateCompletions = 0;
    e->in.lastFrameDone = e->out.lastFrameDone = 0;
    atomic_store(&e->devSampleTime, 0);
    e->nominalPerMs = e->rate / 1000.0;
    e->outRate = e->nominalPerMs;
    e->ticksPerMs = mach_ticks_per_ms();
    e->ticksPerFrame = e->ticksPerMs / e->nominalPerMs;
    e->ztsPeriod = e->rate >= 96000 ? 4096 : 2048;
    e->nextZts = e->ztsPeriod;
    e->seed++;

    bool okOut = e->out.valid && stream_prepare(&e->out);
    bool okIn  = e->in.valid  && stream_prepare(&e->in);
    if (!okOut && !okIn) { LOGE("no stream could be started"); if (e->out.valid) stream_release_buffers(&e->out); if (e->in.valid) stream_release_buffers(&e->in); return; }
    e->captureMaster = okIn;

    stream_t *ref = okIn ? &e->in : &e->out;
    e->anchorFrame = 0; refresh_anchor(e, ref);
    UInt64 f0 = e->anchorFrame + START_LEAD_FRAMES;
    e->in.nextFrame = e->out.nextFrame = f0;
    e->ioStartHost = e->anchorHost + (uint64_t)((double)(f0 - e->anchorFrame) * e->ticksPerMs);
    e->ztsHost = 0;
    zts_publish(e, 0.0, e->ioStartHost);   /* provisional; replaced below */
    e->stampPending = 2;
    atomic_store(&e->running, true);

    IOReturn kr = 0;
    if (okIn)  { e->in.streaming  = true; for (int b = 0; b < NBUF && !kr; b++) kr = submit_xfer(&e->in,  &e->in.xf[b]); }
    if (okOut && !kr) { e->out.streaming = true; for (int b = 0; b < NBUF && !kr; b++) kr = submit_xfer(&e->out, &e->out.xf[b]); }
    if (kr) {
        LOGE("initial submit failed 0x%x", kr);
        atomic_store(&e->running, false);
        stream_stop(&e->in); stream_stop(&e->out);
        *res = kr; return;
    }
    /* wait (≤100 ms) for the timeline origin to be fixed from real frame timestamps */
    for (int i = 0; i < 50 && e->stampPending > 0; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
    if (e->stampPending > 0) LOGE("no completions within 100 ms; using extrapolated origin");
    LOG("IO started: rate=%u in=%d out=%d captureMaster=%d nf=%u nbuf=%u f0=%llu", e->rate, okIn, okOut, e->captureMaster, e->nf, e->nbuf, (unsigned long long)f0);
    *res = 0;
}

static void job_stop(engine_t *e, void *arg) {
    (void)arg;
    if (!atomic_load(&e->running)) return;
    atomic_store(&e->running, false);
    stream_stop(&e->in); stream_stop(&e->out);
    LOG("IO stopped: rx=%llu tx=%llu rxErr=%llu txErr=%llu resyncs=%llu snaps=%u rate=%.3f rxPerMs=%.5f outRate=%.5f fbErr=%d maxLat=%.0fus late=%u", (unsigned long long)e->rxCompleted, (unsigned long long)e->txCompleted,
        (unsigned long long)e->rxErrors, (unsigned long long)e->txErrors, (unsigned long long)e->resyncs, e->snaps, e->measuredRate,
        e->rxBusFrames ? (double)e->rxCompleted / (double)e->rxBusFrames : 0.0, e->outRate, e->feedbackErr, e->maxCompletionLatUs, e->lateCompletions);
}

int ua4fx_engine_start(engine_t *e) {
    int res = 0;
    engine_sync(e, job_start, &res);
    return res;
}
void ua4fx_engine_stop(engine_t *e) { engine_sync(e, job_stop, NULL); }
bool ua4fx_engine_is_running(engine_t *e) { return atomic_load(&e->running); }
bool ua4fx_engine_input_active(engine_t *e)  { return atomic_load(&e->running) && e->in.streaming; }
bool ua4fx_engine_output_active(engine_t *e) { return atomic_load(&e->running) && e->out.streaming; }

/* ------------------------------------------------------------------------- */
/* device attach / detach (USB thread)                                         */

static void device_detach(engine_t *e) {
    if (!e->dev) return;
    LOG("UA-4FX detached");
    if (atomic_load(&e->running)) job_stop(e, NULL);
    atomic_store(&e->present, false);
    atomic_store(&e->rate, 0);
    stream_t *ss[2] = { &e->in, &e->out };
    for (int i = 0; i < 2; i++) {
        stream_t *s = ss[i];
        if (s->src) { CFRunLoopRemoveSource(e->rl, s->src, kCFRunLoopDefaultMode); s->src = NULL; }
        if (s->ifc) { (*s->ifc)->USBInterfaceClose(s->ifc); (*s->ifc)->Release(s->ifc); s->ifc = NULL; }
        s->valid = false;
    }
    if (e->interestNotification) { IOObjectRelease(e->interestNotification); e->interestNotification = 0; }
    (*e->dev)->USBDeviceClose(e->dev); (*e->dev)->Release(e->dev); e->dev = NULL;
    if (e->devService) { IOObjectRelease(e->devService); e->devService = 0; }
    if (e->hotplugCb) e->hotplugCb(e->hotplugCtx, false);
}

static void device_interest(void *refcon, io_service_t service, natural_t messageType, void *arg) {
    (void)service; (void)arg;
    engine_t *e = (engine_t *)refcon;
    if (messageType == kIOMessageServiceIsTerminated) device_detach(e);
}

static bool open_interface(engine_t *e, io_service_t ifs, stream_t *s, const ifinfo_t *info) {
    IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(ifs, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score) || !plug) return false;
    IOUSBInterfaceInterface942 **ifc = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID942), (LPVOID *)&ifc);
    (*plug)->Release(plug);
    if (!ifc) return false;
    IOReturn kr = (*ifc)->USBInterfaceOpen(ifc);
    if (kr) { LOGE("USBInterfaceOpen(%u) failed 0x%x", info->ifnum, kr); (*ifc)->Release(ifc); return false; }
    CFRunLoopSourceRef src = NULL;
    if ((*ifc)->CreateInterfaceAsyncEventSource(ifc, &src) || !src) { (*ifc)->USBInterfaceClose(ifc); (*ifc)->Release(ifc); return false; }
    CFRunLoopAddSource(e->rl, src, kCFRunLoopDefaultMode);
    memset(s, 0, sizeof *s);
    s->e = e; s->ifc = ifc; s->src = src; s->ifnum = info->ifnum; s->epAddr = info->epAddr; s->maxPacket = info->maxPacket;
    s->isInput = (info->role == ROLE_PCM_IN); s->valid = true;
    return true;
}

static void device_attach(engine_t *e, io_service_t svc) {
    if (e->dev) { IOObjectRelease(svc); return; }   /* only one device supported */
    IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score) || !plug) { LOGE("device plugin failed"); IOObjectRelease(svc); return; }
    IOUSBDeviceInterface942 **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID942), (LPVOID *)&dev);
    (*plug)->Release(plug);
    if (!dev) { IOObjectRelease(svc); return; }

    IOReturn kr = (*dev)->USBDeviceOpen(dev);
    if (kr) { LOGE("USBDeviceOpen failed 0x%x (another driver/app holds the device?)", kr); (*dev)->Release(dev); IOObjectRelease(svc); return; }
    UInt8 cfg = 0; (*dev)->GetConfiguration(dev, &cfg);
    if (cfg != 1) { kr = (*dev)->SetConfiguration(dev, 1); if (kr) { LOGE("SetConfiguration failed 0x%x", kr); (*dev)->USBDeviceClose(dev); (*dev)->Release(dev); IOObjectRelease(svc); return; } }
    (*dev)->GetLocationID(dev, &e->locationID);
    e->serial[0] = 0;
    { UInt8 idx = 0; if ((*dev)->USBGetSerialNumberStringIndex(dev, &idx) == 0 && idx) {
        IOUSBDevRequest r; uint8_t buf[256] = {0};
        r.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice); r.bRequest = kUSBRqGetDescriptor; r.wValue = (kUSBStringDesc << 8) | idx; r.wIndex = 0x0409; r.wLength = 255; r.pData = buf;
        if ((*dev)->DeviceRequest(dev, &r) == 0 && buf[0] > 2) { int n = 0; for (int i = 2; i < buf[0] && n < 63; i += 2) e->serial[n++] = (char)buf[i]; e->serial[n] = 0; } } }

    ifinfo_t infos[8]; int ni = parse_config(dev, infos, 8);
    uint32_t rate = 0; e->midiIfnum = 0xFF;
    for (int i = 0; i < ni; i++) {
        if (infos[i].role == ROLE_PCM_IN || infos[i].role == ROLE_PCM_OUT) {
            uint32_t r = rate_from_maxpacket(infos[i].maxPacket);
            if (!r) r = infos[i].descRate;
            if (r) rate = r;
        } else if (infos[i].role == ROLE_MIDI) e->midiIfnum = infos[i].ifnum;
    }
    if (!rate) { LOGE("could not determine sample rate from descriptors"); (*dev)->USBDeviceClose(dev); (*dev)->Release(dev); IOObjectRelease(svc); return; }

    e->dev = dev; e->devService = svc;
    /* open PCM interfaces */
    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t it = 0;
    if ((*dev)->CreateInterfaceIterator(dev, &req, &it) == 0) {
        io_service_t ifs;
        while ((ifs = IOIteratorNext(it))) {
            CFTypeRef num = IORegistryEntryCreateCFProperty(ifs, CFSTR("bInterfaceNumber"), kCFAllocatorDefault, 0);
            int ifnum = -1; if (num) { CFNumberGetValue(num, kCFNumberIntType, &ifnum); CFRelease(num); }
            for (int i = 0; i < ni; i++) {
                if (infos[i].ifnum != ifnum) continue;
                if (infos[i].role == ROLE_PCM_IN  && !e->in.valid)  open_interface(e, ifs, &e->in,  &infos[i]);
                if (infos[i].role == ROLE_PCM_OUT && !e->out.valid) open_interface(e, ifs, &e->out, &infos[i]);
            }
            IOObjectRelease(ifs);
        }
        IOObjectRelease(it);
    }
    if (!e->in.valid && !e->out.valid) { LOGE("no PCM interface could be opened"); device_detach(e); return; }

    IOServiceAddInterestNotification(e->notifyPort, svc, kIOGeneralInterest, device_interest, e, &e->interestNotification);
    atomic_store(&e->rate, rate);
    atomic_store(&e->present, true);
    LOG("UA-4FX attached: rate=%u in=%s(if %u, ep 0x%02x, mps %u) out=%s(if %u, ep 0x%02x, mps %u) midiIf=%d loc=0x%08x",
        rate, e->in.valid ? "yes" : "no", e->in.ifnum, e->in.epAddr, e->in.maxPacket,
        e->out.valid ? "yes" : "no", e->out.ifnum, e->out.epAddr, e->out.maxPacket, e->midiIfnum == 0xFF ? -1 : e->midiIfnum, e->locationID);
    if (e->hotplugCb) e->hotplugCb(e->hotplugCtx, true);
}

static void devices_added(void *refcon, io_iterator_t it) {
    engine_t *e = (engine_t *)refcon; io_service_t svc;
    while ((svc = IOIteratorNext(it))) device_attach(e, svc);   /* device_attach owns/releases svc */
}
static void devices_removed(void *refcon, io_iterator_t it) {
    engine_t *e = (engine_t *)refcon; io_service_t svc;
    while ((svc = IOIteratorNext(it))) { IOObjectRelease(svc); }
    (void)e; /* actual teardown is driven by the per-device interest notification */
}

/* ------------------------------------------------------------------------- */
/* thread                                                                      */

static void *usb_thread(void *arg) {
    engine_t *e = (engine_t *)arg;
    pthread_setname_np("ua4fx-usb");
    e->rtPolicyOK = set_realtime();
    e->rl = CFRunLoopGetCurrent();
    CFRetain(e->rl);

    e->notifyPort = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(e->rl, IONotificationPortGetRunLoopSource(e->notifyPort), kCFRunLoopDefaultMode);
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    int vid = UA4FX_VID, pid = UA4FX_PID_ADVANCED;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid), p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), v); CFDictionarySetValue(m, CFSTR(kUSBProductID), p);
    CFRelease(v); CFRelease(p);
    CFRetain(m);
    IOServiceAddMatchingNotification(e->notifyPort, kIOFirstMatchNotification, m, devices_added, e, &e->addIter);
    IOServiceAddMatchingNotification(e->notifyPort, kIOTerminatedNotification, m, devices_removed, e, &e->removeIter);
    devices_added(e, e->addIter);      /* arm + pick up an already-connected device */
    devices_removed(e, e->removeIter);

    dispatch_semaphore_signal(e->ready);
    while (!atomic_load(&e->quit)) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);

    if (atomic_load(&e->running)) job_stop(e, NULL);
    device_detach(e);
    if (e->addIter) IOObjectRelease(e->addIter);
    if (e->removeIter) IOObjectRelease(e->removeIter);
    IONotificationPortDestroy(e->notifyPort);
    return NULL;
}

ua4fx_engine_t *ua4fx_engine_create(void) {
    engine_t *e = calloc(1, sizeof *e);
    e->inRing = calloc(RING, BPF); e->outRing = calloc(RING, BPF);
    e->nf = UA4FX_DEFAULT_FRAMES_PER_XFER; e->nbuf = UA4FX_DEFAULT_XFERS_IN_FLIGHT;
    e->ready = dispatch_semaphore_create(0);
    e->seed = (uint64_t)mach_absolute_time();
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 512 * 1024);
    if (pthread_create(&e->thread, &a, usb_thread, e)) { LOGE("pthread_create failed"); free(e->inRing); free(e->outRing); free(e); return NULL; }
    pthread_attr_destroy(&a);
    dispatch_semaphore_wait(e->ready, DISPATCH_TIME_FOREVER);
    return e;
}

void ua4fx_engine_destroy(engine_t *e) {
    if (!e) return;
    atomic_store(&e->quit, true);
    CFRunLoopWakeUp(e->rl);
    pthread_join(e->thread, NULL);
    CFRelease(e->rl);
    dispatch_release(e->ready);
    free(e->inRing); free(e->outRing); free(e);
}

void ua4fx_engine_set_hotplug_callback(engine_t *e, ua4fx_hotplug_cb cb, void *ctx) { e->hotplugCb = cb; e->hotplugCtx = ctx; }

/* ------------------------------------------------------------------------- */
/* queries                                                                     */

bool     ua4fx_engine_device_present(engine_t *e) { return atomic_load(&e->present); }
uint32_t ua4fx_engine_sample_rate(engine_t *e)    { return atomic_load(&e->rate); }
uint32_t ua4fx_engine_location_id(engine_t *e)    { return e->locationID; }
const char *ua4fx_engine_serial(engine_t *e)      { return e->serial; }
uint32_t ua4fx_engine_zts_period(engine_t *e)     { uint32_t r = atomic_load(&e->rate); return r >= 96000 ? 4096 : 2048; }

static uint32_t ms_to_frames(engine_t *e, double ms) { return (uint32_t)((double)atomic_load(&e->rate) * ms / 1000.0 + 0.5); }
/* Output: samples for bus frame F are read from the ring when the transfer is
 * queued, (NBUF)*NF ms before F, plus completion latency and margin. */
uint32_t ua4fx_engine_safety_offset_output(engine_t *e) { return ms_to_frames(e, (NBUF + 1) * NF + 1.5); }
/* Input: samples of bus frame F land in the ring at completion, ≤ NF ms + latency later. */
uint32_t ua4fx_engine_safety_offset_input(engine_t *e)  { return ms_to_frames(e, NF + 1.5); }

void ua4fx_engine_set_config(engine_t *e, const ua4fx_config_t *c) {
    uint32_t nf = c->framesPerXfer, nb = c->xfersInFlight;
    if (nf < 1) nf = 1; if (nf > UA4FX_MAX_FRAMES_PER_XFER) nf = UA4FX_MAX_FRAMES_PER_XFER;
    if (nb < 2) nb = 2; if (nb > UA4FX_MAX_XFERS_IN_FLIGHT) nb = UA4FX_MAX_XFERS_IN_FLIGHT;
    e->nf = nf; e->nbuf = nb;    /* picked up by the next start */
}
void ua4fx_engine_get_config(engine_t *e, ua4fx_config_t *c) { c->framesPerXfer = e->nf; c->xfersInFlight = e->nbuf; }
uint32_t ua4fx_engine_latency_input(engine_t *e)  { (void)e; return 8; }
uint32_t ua4fx_engine_latency_output(engine_t *e) { (void)e; return 8; }

bool ua4fx_engine_get_zero_timestamp(engine_t *e, double *sampleTime, uint64_t *hostTime, uint64_t *seed) {
    if (!atomic_load(&e->running)) return false;
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&e->ztsSeq, memory_order_acquire);
        if (s1 & 1) continue;
        double st = e->ztsSample; uint64_t ht = e->ztsHost; uint64_t sd = e->seed;
        uint32_t s2 = atomic_load_explicit(&e->ztsSeq, memory_order_acquire);
        if (s1 == s2) { *sampleTime = st; *hostTime = ht; *seed = sd; return true; }
    }
}

void ua4fx_engine_read_input(engine_t *e, int64_t sampleTime, uint32_t frames, void *dst) {
    uint8_t *d = (uint8_t *)dst;
    uint64_t pos = (uint64_t)sampleTime;
    uint32_t first = (uint32_t)(pos & (RING - 1));
    uint32_t n1 = frames; if (first + n1 > RING) n1 = RING - first;
    memcpy(d, e->inRing + first * BPF, n1 * BPF);
    if (n1 < frames) memcpy(d + n1 * BPF, e->inRing, (frames - n1) * BPF);
}

void ua4fx_engine_write_output(engine_t *e, int64_t sampleTime, uint32_t frames, const void *src) {
    const uint8_t *s = (const uint8_t *)src;
    uint64_t pos = (uint64_t)sampleTime;
    uint32_t first = (uint32_t)(pos & (RING - 1));
    uint32_t n1 = frames; if (first + n1 > RING) n1 = RING - first;
    memcpy(e->outRing + first * BPF, s, n1 * BPF);
    if (n1 < frames) memcpy(e->outRing, s + n1 * BPF, (frames - n1) * BPF);
}

void ua4fx_engine_get_stats(engine_t *e, ua4fx_stats_t *o) {
    memset(o, 0, sizeof *o);
    o->running = atomic_load(&e->running); o->inputActive = o->running && e->in.streaming; o->outputActive = o->running && e->out.streaming;
    o->captureMaster = e->captureMaster; o->framesPerXfer = e->nf; o->xfersInFlight = e->nbuf;
    o->rxFrames = e->rxCompleted; o->txFrames = e->txCompleted;
    o->rxPackets = e->rxPackets; o->txPackets = e->txPackets;
    o->rxErrors = e->rxErrors; o->txErrors = e->txErrors; o->resyncs = e->resyncs;
    o->measuredRate = e->measuredRate; o->feedbackError = e->feedbackErr;
    o->rxPerBusFrame = e->rxBusFrames ? (double)e->rxCompleted / (double)e->rxBusFrames : 0.0;
    o->outRate = e->outRate; o->packetsAdjusted = e->packetsAdjusted;
    o->snaps = e->snaps; o->maxCompletionLatencyUs = e->maxCompletionLatUs; o->lateCompletions = e->lateCompletions; o->rtPolicyOK = e->rtPolicyOK;
}

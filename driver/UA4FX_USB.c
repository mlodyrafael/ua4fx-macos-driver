/*
 * UA4FX_USB.c — user-space isochronous streaming engine for the EDIROL UA-4FX.
 *
 * Design (v0.5, "tick" data path)
 * --------------------------------
 * * USB thread (CFRunLoop): owns every IOKit object, hot-plug, start/stop, and
 *   the isochronous *queue*: transfers are (re)submitted far ahead of the bus
 *   (capture: nbufIn ms, playback: nbufOut ms) so that late completion
 *   callbacks can never make a submission "too old". Completion callbacks do
 *   nothing but hand the slot back to the queue.
 * * Tick thread (real-time, wakes ~200 µs after every 1 ms USB frame boundary):
 *   - harvests capture frames straight from the shared low-latency frame lists
 *     (the host controller updates frStatus / frActCount / frTimeStamp at
 *     primary interrupt time), so input data reaches the ring ~1 ms after the
 *     frame ended regardless of callback latency;
 *   - fills playback data into the already-queued transfer buffers just
 *     `outputLeadMs` before the bus transmits them ("late fill"). The buffers
 *     are DMA-shared with the controller, which is what LowLatencyCreateBuffer
 *     is for. Playback safety offset = lead + 1.5 ms instead of queue depth.
 *   - keeps the device clock (zero timestamps) and the implicit feedback
 *     (playback packet sizes track the ADC rate; the OUT endpoint is adaptive).
 * * Rings are indexed by absolute device sample time. Single producer / single
 *   consumer per ring (HAL IO thread <-> tick thread).
 *
 * The device needs no vendor requests: SET_CONFIGURATION(1), then
 * SET_INTERFACE(alt 1) on the PCM interfaces, then isochronous traffic
 * (same as Linux snd-usb-audio create_uaxx_quirk()).
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
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define RING UA4FX_RING_FRAMES
#define BPF  UA4FX_BYTES_PER_FRAME
#define MAXNF UA4FX_MAX_FRAMES_PER_XFER
#define MAXNB UA4FX_MAX_XFERS_IN_FLIGHT
#define START_LEAD_FRAMES 4          /* first submission this many bus frames ahead */
#define RESYNC_LEAD_FRAMES 4
#define TICK_OFFSET_US 200.0         /* wake this long after a frame boundary */
/* frStatus must be 0 at submission (IOUSBLib rejects other values); completion is detected from
 * frTimeStamp / frActCount, which we zero and the host controller fills in at primary interrupt time. */
#define POSMAP 64                    /* playback position map depth (bus frames) */

typedef struct ua4fx_engine engine_t;

typedef struct xfer {
    IOUSBLowLatencyIsocFrame *fl;
    uint8_t  *buf;
    UInt64    frame;             /* first bus frame of this transfer */
    uint64_t  pos[MAXNF];        /* playback: ring position of each USB frame */
    uint32_t  cnt[MAXNF];        /* playback: audio frames in each USB frame */
    uint32_t  off[MAXNF];        /* playback: byte offset in buf (contiguous packing) */
    uint8_t   done[MAXNF];       /* harvested (in) / filled (out) */
    atomic_bool ready;           /* submitted and fields valid */
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
    bool    valid;               /* interface found and opened */
    bool    streaming;           /* alt 1 selected and transfers queued */
    bool    stopping;
    xfer_t  xf[MAXNB];
    uint32_t nbuf;
    int     inFlight;
    UInt64  nextFrame;           /* next bus frame to schedule */
    os_unfair_lock lock;
} stream_t;

struct ua4fx_engine {
    /* threads */
    pthread_t   thread;          /* USB / run loop thread */
    CFRunLoopRef rl;
    dispatch_semaphore_t ready;
    atomic_bool quit;
    pthread_t   tick;            /* real-time tick thread */
    atomic_bool tickRun;
    bool        tickStarted;

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
    UInt8    midiIfnum;
    stream_t in, out;

    /* geometry (set before start) */
    uint32_t nf, nbuf, nbufOut, outLead;

    /* IO state */
    atomic_bool running;
    uint8_t *inRing;
    uint8_t *outRing;

    /* capture side (tick thread) */
    _Atomic uint64_t rxCompleted;        /* frames harvested into the ring == device clock */
    _Atomic uint64_t lastHarvestFrame;   /* bus frame of the last harvested packet */
    UInt64   nextHarvestFrame;
    uint64_t rxBusFrames, rxPackets, rxErrors, lostInFrames;

    /* playback side */
    uint64_t txSubmitted;                /* USB thread: ring position handed to the queue */
    uint64_t txCompleted, txPackets, txErrors, lostOutFrames;
    UInt64   nextFillFrame;              /* tick thread */
    UInt64   nextTxPollFrame;            /* tick thread: completion polling for stats / clock in nominal mode */
    struct { UInt64 frame; uint64_t posEnd; } posMap[POSMAP];   /* USB thread writes, tick thread reads */
    double   nominalPerMs;
    double   outAccum;
    _Atomic double outRate;              /* frames per bus frame, tick thread -> USB thread */
    atomic_bool realignRequest;
    uint32_t packetsAdjusted, snaps, lateFills, lateHarvests;
    int32_t  feedbackErr;
    bool     captureMaster;
    uint64_t resyncs; uint64_t lastResyncHost;

    /* timing */
    double   ticksPerMs, ticksPerFrame;
    UInt64   anchorFrame; uint64_t anchorHost; uint64_t lastAnchorHost;
    _Atomic uint32_t refSeq; UInt64 refFrame; uint64_t refHost;    /* end-of-frame reference for the tick clock */
    uint32_t ztsPeriod; uint64_t nextZts;
    _Atomic uint32_t ztsSeq; double ztsSample; uint64_t ztsHost; uint64_t seed;
    uint64_t ioStartHost; int stampPending;
    uint64_t rateT0Host, rateN0; double measuredRate;
    double   maxTickLatUs; uint32_t lateTicks;
    bool     rtPolicyOK, tickRtOK;
};

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */

static inline uint64_t at2u64(AbsoluteTime t) { uint64_t v; memcpy(&v, &t, sizeof v); return v; }
static double mach_ticks_per_ms(void) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return 1e6 * (double)tb.denom / (double)tb.numer; }

static bool set_realtime(double periodMs, double compMs, double constrMs) {
    double tpm = mach_ticks_per_ms();
    thread_time_constraint_policy_data_t p;
    p.period = (uint32_t)(tpm * periodMs); p.computation = (uint32_t)(tpm * compMs); p.constraint = (uint32_t)(tpm * constrMs); p.preemptible = 1;
    kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr) LOGE("thread_policy_set failed: %x", kr);
    return kr == KERN_SUCCESS;
}

typedef void (*job_fn)(engine_t *, void *);
static void engine_sync(engine_t *e, job_fn fn, void *arg) {
    if (pthread_equal(pthread_self(), e->thread)) { fn(e, arg); return; }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    CFRunLoopPerformBlock(e->rl, kCFRunLoopDefaultMode, ^{ fn(e, arg); dispatch_semaphore_signal(done); });
    CFRunLoopWakeUp(e->rl);
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);
}

static void ref_publish(engine_t *e, UInt64 frame, uint64_t hostEnd) {
    atomic_fetch_add_explicit(&e->refSeq, 1, memory_order_release);
    e->refFrame = frame; e->refHost = hostEnd;
    atomic_fetch_add_explicit(&e->refSeq, 1, memory_order_release);
}
static void ref_read(engine_t *e, UInt64 *frame, uint64_t *host) {
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&e->refSeq, memory_order_acquire); if (s1 & 1) continue;
        UInt64 f = e->refFrame; uint64_t h = e->refHost;
        uint32_t s2 = atomic_load_explicit(&e->refSeq, memory_order_acquire);
        if (s1 == s2) { *frame = f; *host = h; return; }
    }
}

/* ------------------------------------------------------------------------- */
/* descriptor parsing                                                          */

typedef struct { UInt8 ifnum; UInt8 role; UInt8 epAddr; UInt16 maxPacket; uint32_t descRate; } ifinfo_t;
enum { ROLE_NONE = 0, ROLE_PCM_OUT, ROLE_PCM_IN, ROLE_MIDI };

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
    if (mps <= 0x120 + 6) return 44100;
    if (mps <= 0x140 + 6) return 48000;
    if (mps >= 0x258 - 6 && mps <= 0x260 + 6) return 96000;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* device clock / zero timestamps (tick thread)                                */

static void zts_publish(engine_t *e, double sample, uint64_t host) {
    atomic_fetch_add_explicit(&e->ztsSeq, 1, memory_order_release);
    e->ztsSample = sample; e->ztsHost = host;
    atomic_fetch_add_explicit(&e->ztsSeq, 1, memory_order_release);
}

static void clock_advance(engine_t *e, uint32_t n, uint64_t hostEnd) {
    uint64_t before = atomic_load_explicit(&e->rxCompleted, memory_order_relaxed);
    uint64_t after  = before + n;
    while (after >= e->nextZts) {
        uint64_t host = hostEnd - (uint64_t)((double)(after - e->nextZts) * e->ticksPerFrame);
        if (host <= e->ztsHost) host = e->ztsHost + 1;
        zts_publish(e, (double)e->nextZts, host);
        e->nextZts += e->ztsPeriod;
    }
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
/* queue (USB thread)                                                          */

static void xfer_complete(void *refcon, IOReturn result, void *arg0);

static xfer_t *find_xfer(stream_t *s, UInt64 frame) {
    for (uint32_t b = 0; b < s->nbuf; b++) {
        xfer_t *x = &s->xf[b];
        if (atomic_load_explicit(&x->ready, memory_order_acquire) && frame >= x->frame && frame < x->frame + s->e->nf) return x;
    }
    return NULL;
}
static bool next_ready_frame_after(stream_t *s, UInt64 frame, UInt64 *out) {
    bool found = false; UInt64 best = 0;
    for (uint32_t b = 0; b < s->nbuf; b++) {
        xfer_t *x = &s->xf[b];
        if (!atomic_load_explicit(&x->ready, memory_order_acquire) || x->frame <= frame) continue;
        if (!found || x->frame < best) { best = x->frame; found = true; }
    }
    if (found) *out = best;
    return found;
}

/* Called with s->lock held. Assigns positions/sizes (playback) and queues the transfer. */
static IOReturn submit_xfer(stream_t *s, xfer_t *x) {
    engine_t *e = s->e; uint32_t nf = e->nf;
    x->frame = s->nextFrame;
    memset(x->done, 0, sizeof x->done);
    if (s->isInput) {
        for (uint32_t i = 0; i < nf; i++) { x->fl[i].frReqCount = s->maxPacket; x->fl[i].frActCount = 0; x->fl[i].frStatus = 0; memset(&x->fl[i].frTimeStamp, 0, sizeof(AbsoluteTime)); }
    } else {
        if (atomic_exchange(&e->realignRequest, false) && e->captureMaster) {
            /* put the read pointer where the capture timeline says this frame will be */
            uint64_t rx = atomic_load(&e->rxCompleted); UInt64 lf = atomic_load(&e->lastHarvestFrame);
            double ahead = (double)((int64_t)x->frame - (int64_t)lf - 1); if (ahead < 0) ahead = 0;
            e->txSubmitted = rx + (uint64_t)(ahead * e->nominalPerMs); e->outAccum = 0; e->snaps++;
        }
        uint32_t off = 0; double rate = atomic_load(&e->outRate); uint32_t maxN = s->maxPacket / BPF;
        for (uint32_t i = 0; i < nf; i++) {
            e->outAccum += rate; uint32_t n = (uint32_t)e->outAccum; e->outAccum -= n;
            if (n > maxN) n = maxN;
            if (n != (uint32_t)e->nominalPerMs) e->packetsAdjusted++;
            x->pos[i] = e->txSubmitted; x->cnt[i] = n; x->off[i] = off; off += n * BPF;
            e->txSubmitted += n;
            x->fl[i].frReqCount = n * BPF; x->fl[i].frActCount = 0; x->fl[i].frStatus = 0; memset(&x->fl[i].frTimeStamp, 0, sizeof(AbsoluteTime));
            e->posMap[(x->frame + i) % POSMAP].frame = x->frame + i; e->posMap[(x->frame + i) % POSMAP].posEnd = e->txSubmitted;
        }
        memset(x->buf, 0, off);                /* silence unless the tick thread fills it in time */
    }
    IOReturn kr = s->isInput
        ? (*s->ifc)->LowLatencyReadIsochPipeAsync (s->ifc, s->pipe, x->buf, x->frame, nf, 1, x->fl, xfer_complete, x)
        : (*s->ifc)->LowLatencyWriteIsochPipeAsync(s->ifc, s->pipe, x->buf, x->frame, nf, 1, x->fl, xfer_complete, x);
    if (kr == kIOReturnSuccess) { x->inFlight = true; s->inFlight++; s->nextFrame += nf; atomic_store_explicit(&x->ready, true, memory_order_release); }
    return kr;
}

/* Called with s->lock held: move this stream's schedule forward after falling behind. */
static void stream_resync(stream_t *s, const char *why) {
    engine_t *e = s->e; UInt64 f = 0; AbsoluteTime at;
    if ((*s->ifc)->GetBusFrameNumberWithTime(s->ifc, &f, &at) != 0) return;
    UInt64 R = f + RESYNC_LEAD_FRAMES;
    if (R > s->nextFrame) { if (s->isInput) e->lostInFrames += R - s->nextFrame; else e->lostOutFrames += R - s->nextFrame; s->nextFrame = R; }
    if (!s->isInput) atomic_store(&e->realignRequest, true);
    e->resyncs++;
    uint64_t now = mach_absolute_time();
    if (now - e->lastResyncHost > (uint64_t)(e->ticksPerMs * 5000.0)) {
        e->lastResyncHost = now;
        LOGE("resync #%llu (%s on %s): bus %llu -> next %llu; tickLatMax=%.0fus lateTicks=%u lateFills=%u", (unsigned long long)e->resyncs, why, s->isInput ? "in" : "out",
             (unsigned long long)f, (unsigned long long)s->nextFrame, e->maxTickLatUs, e->lateTicks, e->lateFills);
    }
}

static IOReturn stream_fill_queue_locked(stream_t *s) {
    IOReturn last = 0;
    for (uint32_t b = 0; b < s->nbuf; b++) if (!s->xf[b].inFlight) {
        IOReturn kr = submit_xfer(s, &s->xf[b]);
        if (kr == kIOReturnIsoTooOld || kr == kIOReturnIsoTooNew) { stream_resync(s, kr == kIOReturnIsoTooOld ? "IsoTooOld" : "IsoTooNew"); kr = submit_xfer(s, &s->xf[b]); }
        if (kr) last = kr;
    }
    return last;
}

static void harvest_locked(engine_t *e, UInt64 current);   /* tick-thread work, also usable from the callback under in.lock */

static void xfer_complete(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    xfer_t *x = (xfer_t *)refcon; stream_t *s = x->stream; engine_t *e = s->e;
    os_unfair_lock_lock(&s->lock);
    x->inFlight = false; s->inFlight--;
    if (s->stopping || !atomic_load(&e->running)) { atomic_store(&x->ready, false); os_unfair_lock_unlock(&s->lock); return; }
    if (result == kIOReturnAborted || result == kIOReturnNotResponding || result == kIOReturnNoDevice) { atomic_store(&x->ready, false); os_unfair_lock_unlock(&s->lock); return; }
    if (s->isInput) {
        harvest_locked(e, UINT64_MAX);            /* the transfer is complete: take whatever is left */
        for (uint32_t i = 0; i < e->nf; i++) if (!x->done[i]) e->lateHarvests++;
    } else {
        for (uint32_t i = 0; i < e->nf; i++) { if (x->fl[i].frStatus != kIOReturnSuccess && x->fl[i].frStatus != kIOReturnUnderrun) e->txErrors++; }
    }
    atomic_store_explicit(&x->ready, false, memory_order_release);
    IOReturn kr = stream_fill_queue_locked(s);
    if (kr) LOGE("%s requeue failed: 0x%x", s->isInput ? "in" : "out", kr);
    os_unfair_lock_unlock(&s->lock);
}

/* ------------------------------------------------------------------------- */
/* tick thread: harvest capture, fill playback                                 */

/* A frame is finished when the HC stamped it (or delivered data), or — for frames that
 * errored without either — when it ended ≥ 3 bus frames ago. */
static bool frame_finished(const IOUSBLowLatencyIsocFrame *f, UInt64 frame, UInt64 current) {
    if (at2u64(f->frTimeStamp) != 0 || f->frActCount != 0) return true;
    return current == UINT64_MAX || (current > frame && current - frame >= 3);
}

/* in.lock held */
static void harvest_locked(engine_t *e, UInt64 current) {
    stream_t *s = &e->in;
    for (;;) {
        xfer_t *x = find_xfer(s, e->nextHarvestFrame);
        if (!x) {
            UInt64 nf;
            if (next_ready_frame_after(s, e->nextHarvestFrame, &nf)) { e->lostInFrames += nf - e->nextHarvestFrame; e->nextHarvestFrame = nf; continue; }
            return;
        }
        uint32_t i = (uint32_t)(e->nextHarvestFrame - x->frame);
        if (x->done[i]) { e->nextHarvestFrame++; continue; }
        IOUSBLowLatencyIsocFrame *f = &x->fl[i];
        if (!frame_finished(f, e->nextHarvestFrame, current)) return;   /* HC has not finished this frame yet */
        IOReturn st = f->frStatus;
        uint32_t n = (st == kIOReturnSuccess || st == kIOReturnUnderrun) ? f->frActCount / BPF : 0;
        if (n == 0 && st != kIOReturnSuccess && st != kIOReturnUnderrun) e->rxErrors++;
        uint64_t hostEnd = at2u64(f->frTimeStamp);
        if (hostEnd == 0) { UInt64 rf; uint64_t rh; ref_read(e, &rf, &rh); hostEnd = rh + (uint64_t)((double)((int64_t)e->nextHarvestFrame - (int64_t)rf) * e->ticksPerMs); }
        if (n) {
            uint64_t pos = atomic_load(&e->rxCompleted);
            const uint8_t *src = x->buf + i * s->maxPacket;
            for (uint32_t k = 0; k < n; k++) memcpy(e->inRing + ((pos + k) & (RING - 1)) * BPF, src + k * BPF, BPF);
        }
        if (e->captureMaster) clock_advance(e, n, hostEnd);
        atomic_fetch_add(&e->rxCompleted, n);
        atomic_store(&e->lastHarvestFrame, e->nextHarvestFrame);
        e->rxPackets++; e->rxBusFrames++;
        if (at2u64(f->frTimeStamp)) ref_publish(e, e->nextHarvestFrame, hostEnd);

        /* implicit feedback against the playback position of the same bus frame */
        if (e->captureMaster && e->out.streaming && e->rxBusFrames >= 200) {
            UInt64 F = e->nextHarvestFrame;
            if (e->posMap[F % POSMAP].frame == F) {
                int64_t err = (int64_t)e->posMap[F % POSMAP].posEnd - (int64_t)atomic_load(&e->rxCompleted);
                e->feedbackErr = (int32_t)err;
                double avg = (double)atomic_load(&e->rxCompleted) / (double)e->rxBusFrames;
                double lo = e->nominalPerMs * 0.998, hi = e->nominalPerMs * 1.002;
                if (avg < lo) avg = lo; else if (avg > hi) avg = hi;
                if (err > (int64_t)(e->nominalPerMs * 3.0) || err < -(int64_t)(e->nominalPerMs * 3.0)) {
                    atomic_store(&e->realignRequest, true); atomic_store(&e->outRate, avg);
                } else {
                    double rate = avg - (double)err / 2000.0;
                    if (rate < lo) rate = lo; else if (rate > hi) rate = hi;
                    atomic_store(&e->outRate, rate);
                }
            }
        }
        /* fix the timeline origin from real timestamps (first packet's stamps are unreliable on XHCI) */
        if (e->stampPending > 0 && at2u64(f->frTimeStamp) && --e->stampPending == 0) {
            uint64_t total = atomic_load(&e->rxCompleted);
            uint64_t host0 = hostEnd - (uint64_t)((double)total * e->ticksPerFrame);
            e->ztsHost = 0; zts_publish(e, 0.0, host0); e->ioStartHost = host0;
        }
        x->done[i] = 1; e->nextHarvestFrame++;
    }
}

/* out.lock held: copy ring data into queued playback buffers up to bus frame `target` */
static void fill_locked(engine_t *e, UInt64 target, UInt64 current) {
    stream_t *s = &e->out;
    if (e->nextFillFrame < current + 1) { e->lateFills += (uint32_t)(current + 1 - e->nextFillFrame); e->nextFillFrame = current + 1; }
    while (e->nextFillFrame <= target) {
        xfer_t *x = find_xfer(s, e->nextFillFrame);
        if (!x) {
            UInt64 nf;
            if (next_ready_frame_after(s, e->nextFillFrame, &nf) && nf <= target) { e->nextFillFrame = nf; continue; }
            return;                                                   /* not queued yet: try next tick */
        }
        uint32_t i = (uint32_t)(e->nextFillFrame - x->frame);
        if (!x->done[i]) {
            uint32_t n = x->cnt[i]; uint64_t pos = x->pos[i]; uint8_t *dst = x->buf + x->off[i];
            for (uint32_t k = 0; k < n; k++) {
                uint8_t *src = e->outRing + ((pos + k) & (RING - 1)) * BPF;
                memcpy(dst + k * BPF, src, BPF);
                memset(src, 0, BPF);
            }
            x->done[i] = 1;
        }
        e->nextFillFrame++;
    }
}

/* out.lock held: account completed playback frames (stats; clock when there is no capture) */
static void poll_tx_locked(engine_t *e, UInt64 current) {
    stream_t *s = &e->out;
    for (;;) {
        xfer_t *x = find_xfer(s, e->nextTxPollFrame);
        if (!x) { UInt64 nf; if (next_ready_frame_after(s, e->nextTxPollFrame, &nf)) { e->nextTxPollFrame = nf; continue; } return; }
        uint32_t i = (uint32_t)(e->nextTxPollFrame - x->frame);
        if (!frame_finished(&x->fl[i], e->nextTxPollFrame, current)) return;
        e->txCompleted += x->cnt[i]; e->txPackets++;
        if (!e->captureMaster) {
            uint64_t hostEnd = at2u64(x->fl[i].frTimeStamp);
            if (hostEnd) { ref_publish(e, e->nextTxPollFrame, hostEnd); clock_advance(e, x->cnt[i], hostEnd); atomic_fetch_add(&e->rxCompleted, x->cnt[i]);
                if (e->stampPending > 0 && --e->stampPending == 0) { uint64_t total = atomic_load(&e->rxCompleted); uint64_t host0 = hostEnd - (uint64_t)((double)total * e->ticksPerFrame); e->ztsHost = 0; zts_publish(e, 0.0, host0); e->ioStartHost = host0; } }
        }
        e->nextTxPollFrame++;
    }
}

static void *tick_thread(void *arg) {
    engine_t *e = (engine_t *)arg;
    pthread_setname_np("ua4fx-tick");
    e->tickRtOK = set_realtime(1.0, 0.1, 0.3);
    const double tpm = e->ticksPerMs;
    uint64_t plannedWake = 0;
    while (atomic_load(&e->tickRun)) {
        uint64_t now = mach_absolute_time();
        if (plannedWake) { double lat = ((double)now - (double)plannedWake) / tpm * 1000.0; if (lat > e->maxTickLatUs) e->maxTickLatUs = lat; else e->maxTickLatUs *= 0.9995; if (lat > 700.0) e->lateTicks++; }
        UInt64 rf; uint64_t rh; ref_read(e, &rf, &rh);
        /* frame `rf` ended at `rh`; the frame in progress now: */
        int64_t k = (int64_t)(((double)now - (double)rh) / tpm); if (k < 0) k = 0;
        UInt64 current = rf + 1 + (UInt64)k;

        if (e->in.streaming) { os_unfair_lock_lock(&e->in.lock); harvest_locked(e, current); os_unfair_lock_unlock(&e->in.lock); }
        if (e->out.streaming) { os_unfair_lock_lock(&e->out.lock); poll_tx_locked(e, current); fill_locked(e, current + e->outLead, current); os_unfair_lock_unlock(&e->out.lock); }

        /* sleep until shortly after the next frame boundary */
        ref_read(e, &rf, &rh);
        double sinceRef = (double)mach_absolute_time() - (double)rh;
        int64_t kk = (int64_t)(sinceRef / tpm) + 1;
        plannedWake = rh + (uint64_t)((double)kk * tpm + TICK_OFFSET_US / 1000.0 * tpm);
        mach_wait_until(plannedWake);
    }
    return NULL;
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
    s->nbuf = s->isInput ? e->nbuf : e->nbufOut;
    for (uint32_t b = 0; b < s->nbuf; b++) {
        xfer_t *x = &s->xf[b]; x->idx = (int)b; x->stream = s; x->inFlight = false; atomic_store(&x->ready, false);
        kr = (*s->ifc)->LowLatencyCreateBuffer(s->ifc, (void **)&x->buf, s->maxPacket * e->nf, s->isInput ? kUSBLowLatencyReadBuffer : kUSBLowLatencyWriteBuffer);
        if (kr) { LOGE("LowLatencyCreateBuffer failed 0x%x", kr); return false; }
        kr = (*s->ifc)->LowLatencyCreateBuffer(s->ifc, (void **)&x->fl, sizeof(IOUSBLowLatencyIsocFrame) * e->nf, kUSBLowLatencyFrameListBuffer);
        if (kr) { LOGE("LowLatencyCreateBuffer(fl) failed 0x%x", kr); return false; }
        memset(x->buf, 0, s->maxPacket * e->nf);
    }
    s->stopping = false; s->inFlight = 0;
    return true;
}

static void stream_release_buffers(stream_t *s) {
    for (uint32_t b = 0; b < MAXNB; b++) {
        xfer_t *x = &s->xf[b];
        if (x->buf) { (*s->ifc)->LowLatencyDestroyBuffer(s->ifc, x->buf); x->buf = NULL; }
        if (x->fl)  { (*s->ifc)->LowLatencyDestroyBuffer(s->ifc, x->fl);  x->fl  = NULL; }
        atomic_store(&x->ready, false);
    }
}

static void stream_stop(stream_t *s) {
    if (!s->valid) return;
    os_unfair_lock_lock(&s->lock); s->stopping = true; os_unfair_lock_unlock(&s->lock);
    if (s->streaming) {
        (*s->ifc)->AbortPipe(s->ifc, s->pipe);
        for (int i = 0; i < 200 && s->inFlight > 0; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        if (s->inFlight) LOGE("%s: %d transfers still in flight after abort", s->isInput ? "in" : "out", s->inFlight);
        (*s->ifc)->SetAlternateInterface(s->ifc, 0);
        s->streaming = false;
    }
    stream_release_buffers(s);
}

/* ------------------------------------------------------------------------- */
/* start / stop                                                                */

static void refresh_anchor(engine_t *e, stream_t *s) {
    UInt64 f = 0; AbsoluteTime at;
    if ((*s->ifc)->GetBusFrameNumberWithTime(s->ifc, &f, &at) == 0) { e->anchorFrame = f; e->anchorHost = at2u64(at); e->lastAnchorHost = mach_absolute_time(); }
}

static void job_start(engine_t *e, void *arg) {
    int *res = (int *)arg; *res = kIOReturnNoDevice;
    if (!atomic_load(&e->present)) return;
    if (atomic_load(&e->running)) { *res = 0; return; }

    memset(e->inRing, 0, RING * BPF); memset(e->outRing, 0, RING * BPF);
    atomic_store(&e->rxCompleted, 0); atomic_store(&e->lastHarvestFrame, 0);
    e->txSubmitted = e->txCompleted = 0; e->rxPackets = e->txPackets = e->rxErrors = e->txErrors = 0;
    e->rxBusFrames = 0; e->lostInFrames = e->lostOutFrames = 0; e->packetsAdjusted = e->snaps = e->lateFills = e->lateHarvests = 0;
    e->feedbackErr = 0; e->rateT0Host = 0; e->measuredRate = 0; e->maxTickLatUs = 0; e->lateTicks = 0;
    memset(e->posMap, 0, sizeof e->posMap); atomic_store(&e->realignRequest, false);
    e->outAccum = 0; e->nominalPerMs = e->rate / 1000.0; atomic_store(&e->outRate, e->nominalPerMs);
    e->ticksPerMs = mach_ticks_per_ms(); e->ticksPerFrame = e->ticksPerMs / e->nominalPerMs;
    e->ztsPeriod = e->rate >= 96000 ? 4096 : 2048; e->nextZts = e->ztsPeriod;
    e->seed++;

    bool okOut = e->out.valid && stream_prepare(&e->out);
    bool okIn  = e->in.valid  && stream_prepare(&e->in);
    if (!okOut && !okIn) { LOGE("no stream could be started"); if (e->out.valid) stream_release_buffers(&e->out); if (e->in.valid) stream_release_buffers(&e->in); return; }
    e->captureMaster = okIn;

    stream_t *ref = okIn ? &e->in : &e->out;
    refresh_anchor(e, ref);
    UInt64 f0 = e->anchorFrame + START_LEAD_FRAMES;
    e->in.nextFrame = e->out.nextFrame = f0;
    e->nextHarvestFrame = e->nextFillFrame = e->nextTxPollFrame = f0;
    /* provisional clock reference: anchor time is "somewhere inside" anchorFrame */
    ref_publish(e, e->anchorFrame, e->anchorHost + (uint64_t)(e->ticksPerMs * 0.5));
    e->ioStartHost = e->anchorHost + (uint64_t)((double)(f0 - e->anchorFrame) * e->ticksPerMs);
    e->ztsHost = 0; zts_publish(e, 0.0, e->ioStartHost);
    e->stampPending = 2;
    atomic_store(&e->running, true);

    IOReturn kr = 0;
    if (okIn)  { e->in.streaming  = true; os_unfair_lock_lock(&e->in.lock);  kr = stream_fill_queue_locked(&e->in);  os_unfair_lock_unlock(&e->in.lock); }
    if (okOut && !kr) { e->out.streaming = true; os_unfair_lock_lock(&e->out.lock); kr = stream_fill_queue_locked(&e->out); os_unfair_lock_unlock(&e->out.lock); }
    if (kr) {
        LOGE("initial submit failed 0x%x", kr);
        atomic_store(&e->running, false);
        stream_stop(&e->in); stream_stop(&e->out);
        *res = kr; return;
    }
    atomic_store(&e->tickRun, true);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256 * 1024);
    if (pthread_create(&e->tick, &a, tick_thread, e) == 0) e->tickStarted = true; else LOGE("tick thread creation failed");
    pthread_attr_destroy(&a);
    for (int i = 0; i < 50 && e->stampPending > 0; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
    if (e->stampPending > 0) LOGE("no frames within 100 ms; using extrapolated origin");
    LOG("IO started: rate=%u in=%d out=%d captureMaster=%d nf=%u nbufIn=%u nbufOut=%u lead=%u f0=%llu tickRT=%d", e->rate, okIn, okOut, e->captureMaster, e->nf, e->nbuf, e->nbufOut, e->outLead, (unsigned long long)f0, e->tickRtOK);
    *res = 0;
}

static void job_stop(engine_t *e, void *arg) {
    (void)arg;
    if (!atomic_load(&e->running)) return;
    atomic_store(&e->running, false);
    atomic_store(&e->tickRun, false);
    if (e->tickStarted) { pthread_join(e->tick, NULL); e->tickStarted = false; }
    stream_stop(&e->in); stream_stop(&e->out);
    LOG("IO stopped: rx=%llu tx=%llu rxErr=%llu txErr=%llu lostIn=%llu lostOut=%llu resyncs=%llu snaps=%u rate=%.3f outRate=%.5f fbErr=%d tickLatMax=%.0fus lateTicks=%u lateFills=%u lateHarvests=%u",
        (unsigned long long)atomic_load(&e->rxCompleted), (unsigned long long)e->txCompleted, (unsigned long long)e->rxErrors, (unsigned long long)e->txErrors,
        (unsigned long long)e->lostInFrames, (unsigned long long)e->lostOutFrames, (unsigned long long)e->resyncs, e->snaps, e->measuredRate, atomic_load(&e->outRate), e->feedbackErr,
        e->maxTickLatUs, e->lateTicks, e->lateFills, e->lateHarvests);
}

int  ua4fx_engine_start(engine_t *e) { int res = 0; engine_sync(e, job_start, &res); return res; }
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
    if (messageType == kIOMessageServiceIsTerminated) device_detach((engine_t *)refcon);
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
    s->isInput = (info->role == ROLE_PCM_IN); s->valid = true; s->lock = OS_UNFAIR_LOCK_INIT;
    return true;
}

static void device_attach(engine_t *e, io_service_t svc) {
    if (e->dev) { IOObjectRelease(svc); return; }
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
        if (infos[i].role == ROLE_PCM_IN || infos[i].role == ROLE_PCM_OUT) { uint32_t r = rate_from_maxpacket(infos[i].maxPacket); if (!r) r = infos[i].descRate; if (r) rate = r; }
        else if (infos[i].role == ROLE_MIDI) e->midiIfnum = infos[i].ifnum;
    }
    if (!rate) { LOGE("could not determine sample rate from descriptors"); (*dev)->USBDeviceClose(dev); (*dev)->Release(dev); IOObjectRelease(svc); return; }

    e->dev = dev; e->devService = svc;
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
        rate, e->in.valid ? "yes" : "no", e->in.ifnum, e->in.epAddr, e->in.maxPacket, e->out.valid ? "yes" : "no", e->out.ifnum, e->out.epAddr, e->out.maxPacket,
        e->midiIfnum == 0xFF ? -1 : e->midiIfnum, e->locationID);
    if (e->hotplugCb) e->hotplugCb(e->hotplugCtx, true);
}

static void devices_added(void *refcon, io_iterator_t it) { engine_t *e = (engine_t *)refcon; io_service_t svc; while ((svc = IOIteratorNext(it))) device_attach(e, svc); }
static void devices_removed(void *refcon, io_iterator_t it) { (void)refcon; io_service_t svc; while ((svc = IOIteratorNext(it))) IOObjectRelease(svc); }

/* ------------------------------------------------------------------------- */
/* USB thread                                                                  */

static void *usb_thread(void *arg) {
    engine_t *e = (engine_t *)arg;
    pthread_setname_np("ua4fx-usb");
    e->rtPolicyOK = set_realtime(1.0, 0.15, 0.5);
    e->rl = CFRunLoopGetCurrent(); CFRetain(e->rl);
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
    devices_added(e, e->addIter);
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
    e->nf = UA4FX_DEFAULT_FRAMES_PER_XFER; e->nbuf = UA4FX_DEFAULT_XFERS_IN_FLIGHT; e->nbufOut = UA4FX_DEFAULT_XFERS_IN_FLIGHT_OUT; e->outLead = UA4FX_DEFAULT_OUTPUT_LEAD_MS;
    e->in.lock = OS_UNFAIR_LOCK_INIT; e->out.lock = OS_UNFAIR_LOCK_INIT;
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
/* Output: frame F is filled at the tick after frame F - lead starts, i.e. ~lead ms before it plays. */
uint32_t ua4fx_engine_safety_offset_output(engine_t *e) { return ms_to_frames(e, (double)e->outLead + 1.5); }
/* Input: frame F is harvested at the tick right after it ends (≤ ~1.2 ms), plus margin. */
uint32_t ua4fx_engine_safety_offset_input(engine_t *e)  { return ms_to_frames(e, (double)e->nf + 1.5); }

void ua4fx_engine_set_config(engine_t *e, const ua4fx_config_t *c) {
    uint32_t nf = c->framesPerXfer, nb = c->xfersInFlight, no = c->xfersInFlightOut, lead = c->outputLeadMs;
    if (nf < 1) nf = 1; if (nf > MAXNF) nf = MAXNF;
    if (nb < 2) nb = 2; if (nb > MAXNB) nb = MAXNB;
    if (no < 2) no = 2; if (no > MAXNB) no = MAXNB;
    if (lead < 1) lead = 1; if (lead > no * nf - 1) lead = no * nf - 1;
    e->nf = nf; e->nbuf = nb; e->nbufOut = no; e->outLead = lead;
}
void ua4fx_engine_get_config(engine_t *e, ua4fx_config_t *c) { c->framesPerXfer = e->nf; c->xfersInFlight = e->nbuf; c->xfersInFlightOut = e->nbufOut; c->outputLeadMs = e->outLead; }
uint32_t ua4fx_engine_latency_input(engine_t *e)  { (void)e; return 8; }
uint32_t ua4fx_engine_latency_output(engine_t *e) { (void)e; return 8; }

bool ua4fx_engine_get_zero_timestamp(engine_t *e, double *sampleTime, uint64_t *hostTime, uint64_t *seed) {
    if (!atomic_load(&e->running)) return false;
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&e->ztsSeq, memory_order_acquire); if (s1 & 1) continue;
        double st = e->ztsSample; uint64_t ht = e->ztsHost; uint64_t sd = e->seed;
        uint32_t s2 = atomic_load_explicit(&e->ztsSeq, memory_order_acquire);
        if (s1 == s2) { *sampleTime = st; *hostTime = ht; *seed = sd; return true; }
    }
}

void ua4fx_engine_read_input(engine_t *e, int64_t sampleTime, uint32_t frames, void *dst) {
    uint8_t *d = (uint8_t *)dst; uint64_t pos = (uint64_t)sampleTime;
    uint32_t first = (uint32_t)(pos & (RING - 1)); uint32_t n1 = frames; if (first + n1 > RING) n1 = RING - first;
    memcpy(d, e->inRing + first * BPF, n1 * BPF);
    if (n1 < frames) memcpy(d + n1 * BPF, e->inRing, (frames - n1) * BPF);
}
void ua4fx_engine_write_output(engine_t *e, int64_t sampleTime, uint32_t frames, const void *src) {
    const uint8_t *s = (const uint8_t *)src; uint64_t pos = (uint64_t)sampleTime;
    uint32_t first = (uint32_t)(pos & (RING - 1)); uint32_t n1 = frames; if (first + n1 > RING) n1 = RING - first;
    memcpy(e->outRing + first * BPF, s, n1 * BPF);
    if (n1 < frames) memcpy(e->outRing, s + n1 * BPF, (frames - n1) * BPF);
}

void ua4fx_engine_get_stats(engine_t *e, ua4fx_stats_t *o) {
    memset(o, 0, sizeof *o);
    o->running = atomic_load(&e->running); o->inputActive = o->running && e->in.streaming; o->outputActive = o->running && e->out.streaming;
    o->captureMaster = e->captureMaster; o->framesPerXfer = e->nf; o->xfersInFlight = e->nbuf; o->xfersInFlightOut = e->nbufOut; o->outputLeadMs = e->outLead;
    o->rxFrames = atomic_load(&e->rxCompleted); o->txFrames = e->txCompleted;
    o->rxPackets = e->rxPackets; o->txPackets = e->txPackets;
    o->rxErrors = e->rxErrors + e->lostInFrames; o->txErrors = e->txErrors + e->lostOutFrames; o->resyncs = e->resyncs;
    o->measuredRate = e->measuredRate; o->feedbackError = e->feedbackErr;
    o->rxPerBusFrame = e->rxBusFrames ? (double)atomic_load(&e->rxCompleted) / (double)e->rxBusFrames : 0.0;
    o->outRate = atomic_load(&e->outRate); o->packetsAdjusted = e->packetsAdjusted;
    o->snaps = e->snaps; o->maxCompletionLatencyUs = e->maxTickLatUs; o->lateCompletions = e->lateTicks;
    o->lateFills = e->lateFills; o->lateHarvests = e->lateHarvests;
    o->rtPolicyOK = e->rtPolicyOK && (!e->tickStarted || e->tickRtOK);
}

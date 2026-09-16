/*
 * UA4FX_USB.h — user-space USB streaming engine for the EDIROL / Roland UA-4FX
 * (VID 0x0582, PID 0x00A3, "Advanced" driver mode).
 *
 * Runs entirely in user space on top of IOUSBLib (IOKit). No kernel extension,
 * no DriverKit entitlement required. Shared between the CoreAudio HAL plug-in
 * (UA4FX_Driver.c) and the standalone test tool (tools/ua4fx_test.c).
 */
#ifndef UA4FX_USB_H
#define UA4FX_USB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UA4FX_VID           0x0582
#define UA4FX_PID_ADVANCED  0x00A3
#define UA4FX_PID_STANDARD  0x00A4

#define UA4FX_CHANNELS      2
#define UA4FX_BYTES_PER_SAMPLE 3          /* 24-bit packed little-endian */
#define UA4FX_BYTES_PER_FRAME  (UA4FX_CHANNELS * UA4FX_BYTES_PER_SAMPLE)

/* Streaming geometry limits. One USB (full-speed) frame == 1 ms. */
#define UA4FX_MAX_FRAMES_PER_XFER   8     /* ms of audio per isoch transfer   */
#define UA4FX_MAX_XFERS_IN_FLIGHT   8     /* transfers queued per direction    */
#define UA4FX_DEFAULT_FRAMES_PER_XFER 1
#define UA4FX_DEFAULT_XFERS_IN_FLIGHT 5
#define UA4FX_RING_FRAMES           32768 /* power of two, audio frames        */

typedef struct ua4fx_engine ua4fx_engine_t;

/* Called on the engine's USB thread when the device appears / disappears. */
typedef void (*ua4fx_hotplug_cb)(void *ctx, bool arrived);

/* Runtime-tunable geometry. Applied at the next start. */
typedef struct {
    uint32_t framesPerXfer;    /* 1..UA4FX_MAX_FRAMES_PER_XFER (ms per transfer) */
    uint32_t xfersInFlight;    /* 2..UA4FX_MAX_XFERS_IN_FLIGHT                   */
} ua4fx_config_t;

/* Lifecycle */
ua4fx_engine_t *ua4fx_engine_create(void);
void            ua4fx_engine_destroy(ua4fx_engine_t *e);
void            ua4fx_engine_set_hotplug_callback(ua4fx_engine_t *e, ua4fx_hotplug_cb cb, void *ctx);

/* Configuration (clamped to the limits above) */
void ua4fx_engine_set_config(ua4fx_engine_t *e, const ua4fx_config_t *c);
void ua4fx_engine_get_config(ua4fx_engine_t *e, ua4fx_config_t *c);

/* Device info (valid when present) */
bool     ua4fx_engine_device_present(ua4fx_engine_t *e);
uint32_t ua4fx_engine_sample_rate(ua4fx_engine_t *e);      /* 44100 / 48000 / 96000, 0 if absent */
uint32_t ua4fx_engine_location_id(ua4fx_engine_t *e);
const char *ua4fx_engine_serial(ua4fx_engine_t *e);         /* "" if none */

/* IO control (thread-safe, synchronous) */
int  ua4fx_engine_start(ua4fx_engine_t *e);   /* 0 on success, else IOReturn */
void ua4fx_engine_stop(ua4fx_engine_t *e);
bool ua4fx_engine_is_running(ua4fx_engine_t *e);
bool ua4fx_engine_input_active(ua4fx_engine_t *e);   /* capture stream actually streaming */
bool ua4fx_engine_output_active(ua4fx_engine_t *e);

/* Timing for CoreAudio */
uint32_t ua4fx_engine_zts_period(ua4fx_engine_t *e);          /* frames between zero timestamps */
uint32_t ua4fx_engine_safety_offset_input(ua4fx_engine_t *e);  /* frames */
uint32_t ua4fx_engine_safety_offset_output(ua4fx_engine_t *e); /* frames */
uint32_t ua4fx_engine_latency_input(ua4fx_engine_t *e);        /* frames, presentation latency */
uint32_t ua4fx_engine_latency_output(ua4fx_engine_t *e);
/* Most recent zero timestamp. Returns false if IO is not running. */
bool ua4fx_engine_get_zero_timestamp(ua4fx_engine_t *e, double *sampleTime, uint64_t *hostTime, uint64_t *seed);

/* Ring access — called from the HAL IO thread. Data is 24-bit packed LE, 6 bytes/frame. */
void ua4fx_engine_read_input (ua4fx_engine_t *e, int64_t sampleTime, uint32_t frames, void *dst);
void ua4fx_engine_write_output(ua4fx_engine_t *e, int64_t sampleTime, uint32_t frames, const void *src);

/* Diagnostics */
typedef struct {
    bool     running, inputActive, outputActive, captureMaster;
    uint32_t framesPerXfer, xfersInFlight;
    uint64_t rxFrames, txFrames;       /* audio frames */
    uint64_t rxPackets, txPackets;     /* USB packets */
    uint64_t rxErrors, txErrors;       /* frames with frStatus != 0 */
    uint64_t resyncs;                  /* schedule re-anchors after falling behind */
    double   measuredRate;             /* capture frames per second vs. host clock */
    double   rxPerBusFrame;            /* capture frames per USB frame (device vs. SOF clock) */
    double   outRate;                  /* playback frames per USB frame currently used */
    int32_t  feedbackError;            /* tx - rx at the same bus frame, frames */
    uint32_t packetsAdjusted;          /* playback packets that deviated from the nominal size */
    uint32_t snaps;                    /* hard realignments of the playback read pointer */
    double   maxCompletionLatencyUs;   /* decaying max: completion callback vs. frame end */
    uint32_t lateCompletions;          /* completions > 1 ms after the frame ended */
    bool     rtPolicyOK;               /* USB thread got the time-constraint policy */
} ua4fx_stats_t;
void ua4fx_engine_get_stats(ua4fx_engine_t *e, ua4fx_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif

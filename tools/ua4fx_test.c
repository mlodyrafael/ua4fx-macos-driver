/*
 * ua4fx_test — standalone exerciser for the UA-4FX USB engine.
 * Plays the role of the CoreAudio HAL: paces itself with the engine's zero
 * timestamps, writes a test tone into the output ring ahead of the device
 * clock and reads the input ring behind it, printing clock / feedback stats.
 *
 *   ua4fx_test [seconds] [--tone freq] [--silent]
 */
#define UA4FX_LOG_STDERR 1
#include "../driver/UA4FX_USB.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <mach/mach_time.h>

static double ticks_per_sec(void) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return 1e9 * tb.denom / tb.numer; }

int main(int argc, char **argv) {
    int seconds = 5; double tone = 440.0; int silent = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tone") && i + 1 < argc) tone = atof(argv[++i]);
        else if (!strcmp(argv[i], "--silent")) silent = 1;
        else seconds = atoi(argv[i]);
    }
    ua4fx_engine_t *e = ua4fx_engine_create();
    if (!e) { fprintf(stderr, "engine create failed\n"); return 1; }
    for (int i = 0; i < 50 && !ua4fx_engine_device_present(e); i++) usleep(100000);
    if (!ua4fx_engine_device_present(e)) { fprintf(stderr, "UA-4FX (advanced mode) not found\n"); ua4fx_engine_destroy(e); return 1; }
    uint32_t rate = ua4fx_engine_sample_rate(e);
    printf("device present, rate=%u serial='%s' loc=0x%08x\n", rate, ua4fx_engine_serial(e), ua4fx_engine_location_id(e));
    printf("safety in=%u out=%u zts period=%u\n", ua4fx_engine_safety_offset_input(e), ua4fx_engine_safety_offset_output(e), ua4fx_engine_zts_period(e));

    int r = ua4fx_engine_start(e);
    if (r) { fprintf(stderr, "start failed 0x%x\n", r); ua4fx_engine_destroy(e); return 1; }

    const uint32_t buf = 512;                     /* pretend IO buffer size */
    const double tps = ticks_per_sec();
    double phase = 0; int64_t nextOut = -1; int64_t nextIn = -1;
    double lastZts = -1; uint64_t lastHost = 0; int ztsCount = 0; double maxJitterUs = 0; double sumRate = 0; int rateN = 0;
    double inPeak = 0; uint64_t t0 = mach_absolute_time();
    uint8_t *obuf = malloc(buf * UA4FX_BYTES_PER_FRAME), *ibuf = malloc(buf * UA4FX_BYTES_PER_FRAME);
    uint32_t sOut = ua4fx_engine_safety_offset_output(e) + buf, sIn = ua4fx_engine_safety_offset_input(e) + buf;

    while ((double)(mach_absolute_time() - t0) / tps < seconds) {
        double zs; uint64_t zh, seed;
        if (!ua4fx_engine_get_zero_timestamp(e, &zs, &zh, &seed)) break;
        if (zs != lastZts) {
            if (lastZts >= 0 && zh > lastHost) {
                double dt = (double)(zh - lastHost) / tps; double dn = zs - lastZts;
                double instRate = dn / dt; sumRate += instRate; rateN++;
                double j = fabs(dt - dn / rate) * 1e6; if (j > maxJitterUs) maxJitterUs = j;
            }
            lastZts = zs; lastHost = zh; ztsCount++;
        }
        /* current device sample time, extrapolated like the HAL does */
        uint64_t now = mach_absolute_time();
        double nowSample = zs + ((double)now - (double)zh) / tps * rate;
        if (nextOut < 0) { nextOut = (int64_t)nowSample + sOut; nextIn = (int64_t)nowSample - sIn - buf; }
        while ((double)nextOut < nowSample + sOut + buf) {
            for (uint32_t k = 0; k < buf; k++) {
                int32_t v = silent ? 0 : (int32_t)(sin(phase) * 0.1 * 8388607.0); phase += 2 * M_PI * tone / rate;
                uint8_t *p = obuf + k * 6; p[0] = p[3] = v & 0xff; p[1] = p[4] = (v >> 8) & 0xff; p[2] = p[5] = (v >> 16) & 0xff;
            }
            ua4fx_engine_write_output(e, nextOut, buf, obuf); nextOut += buf;
        }
        while ((double)nextIn + buf < nowSample - sIn) {
            ua4fx_engine_read_input(e, nextIn, buf, ibuf); nextIn += buf;
            for (uint32_t k = 0; k < buf * 2; k++) { const uint8_t *p = ibuf + k * 3; int32_t v = (int32_t)((p[0] | (p[1] << 8) | (p[2] << 16)) << 8) >> 8; double a = fabs(v / 8388607.0); if (a > inPeak) inPeak = a; }
        }
        usleep(2000);
    }
    ua4fx_stats_t st; ua4fx_engine_get_stats(e, &st);
    printf("zts: %d stamps, mean rate from stamps=%.3f Hz, max |dt error|=%.1f us\n", ztsCount, rateN ? sumRate / rateN : 0, maxJitterUs);
    printf("rx frames=%llu pkts=%llu err=%llu | tx frames=%llu pkts=%llu err=%llu | resyncs=%llu | measured capture rate=%.3f | fb err=%d\n",
        (unsigned long long)st.rxFrames, (unsigned long long)st.rxPackets, (unsigned long long)st.rxErrors, (unsigned long long)st.txFrames, (unsigned long long)st.txPackets,
        (unsigned long long)st.txErrors, (unsigned long long)st.resyncs, st.measuredRate, st.feedbackError);
    printf("input peak level: %.4f (%.1f dBFS)\n", inPeak, inPeak > 0 ? 20 * log10(inPeak) : -200.0);
    ua4fx_engine_stop(e);
    ua4fx_engine_destroy(e);
    free(obuf); free(ibuf);
    return 0;
}

# UA-4FX driver for modern macOS (Apple silicon)

User-space CoreAudio driver for the **EDIROL / Roland UA-4FX** USB audio interface in
*Advanced* mode (USB `0582:00a3`). Roland's last macOS driver targets OS X 10.11; nothing
official exists for macOS 11+ or Apple silicon. This project makes the unit work again
with 24-bit audio at the rate selected on its rear switch, plus MIDI IN/OUT.

No kernel extension, no DriverKit entitlement, no SIP changes: the audio path is a
CoreAudio HAL plug-in (`AudioServerPlugIn`) that drives the device's isochronous
endpoints directly through IOUSBLib inside coreaudiod's sandbox. MIDI is a small
per-user launch agent bridging USB-MIDI packets to CoreMIDI virtual endpoints.

```
┌────────────┐   Float32 rings   ┌──────────────────┐  IOUSBLib isoch  ┌─────────┐
│ CoreAudio  │ ◄───────────────► │ UA4FX.driver     │ ◄──────────────► │ UA-4FX  │
│ HAL / apps │   zero timestamps │ (HAL plug-in)    │  if0 OUT, if1 IN │ if 0/1  │
└────────────┘                   └──────────────────┘                  │         │
┌────────────┐  custom props     ▲                                     │         │
│ UA4FX      │ ──────────────────┘  'uacf' config / 'uast' stats       │         │
│ Control.app│                                                         │         │
└────────────┘                   ┌──────────────────┐  bulk/interrupt  │         │
┌────────────┐  virtual endpoints│ ua4fx_midid      │ ◄──────────────► │ if 2    │
│ CoreMIDI   │ ◄───────────────► │                  │  USB-MIDI 1.0    └─────────┘
└────────────┘                   └──────────────────┘
```

## Quick start

```bash
curl -fsSL https://raw.githubusercontent.com/mlodyrafael/ua4fx-macos-driver/main/install.sh | bash
```

That clones the repo to `~/ua4fx-macos-driver`, builds it locally (needs the Xcode Command
Line Tools; the script starts their installer if they are missing), asks for your password
once to place the HAL plug-in in `/Library/Audio/Plug-Ins/HAL`, installs the MIDI bridge
launch agent and `UA4FX Control.app`, and restarts coreaudiod. Then plug the UA-4FX in with
its rear switch on **ADVANCED**.

**Does it start by itself?** Yes. The HAL plug-in is loaded by coreaudiod at boot and waits
for the device: plug the unit in at any time and "EDIROL UA-4FX" appears (and disappears
again when you unplug it; hot-unplug is handled cleanly). The MIDI bridge is a per-user
launch agent (`RunAtLoad` + `KeepAlive`), so it starts at login and attaches whenever the
device shows up. Nothing needs to be opened manually; the control app is optional.

Uninstall: `cd ~/ua4fx-macos-driver && make uninstall`.

**Is it "ASIO"?** ASIO is Windows-only. On macOS the native low-latency path is CoreAudio,
and this driver *is* a CoreAudio device: every DAW (Logic, Ableton, Reaper, Pro Tools…) uses it
directly with the buffer size you pick in the DAW. Driver-side latency is set in UA4FX Control.

## Status

| Piece | State |
|---|---|
| USB streaming engine (`driver/UA4FX_USB.c`) | Verified on this machine: 44.1/48 kHz path, capture + playback, 0 frame errors, implicit feedback keeps tx == rx, zero-timestamp jitter ≤ 40 µs |
| HAL plug-in (`driver/UA4FX_Driver.c`) | Installed and verified inside the real coreaudiod on macOS 27: device listed in Audio MIDI Setup, playback + capture through `AudioDeviceIOProc` for minutes with 0 frame errors, several start/stop cycles |
| MIDI bridge (`midi/ua4fx_midid.c`) | Attaches to the MIDI interface and publishes "UA-4FX MIDI In/Out"; packet coding is standard USB-MIDI. Not yet exercised with a MIDI instrument |
| 96 kHz modes | Untested (switch was at 48 kHz). Designed for: half-duplex; the engine keeps whichever direction the device offers |
| Volume / mute | Two layers: a *trim* set in UA4FX Control (default output +7 dB, input 0 dB) plus the macOS volume control (0…−60 dB, slider / media keys) exposed as HAL control objects. Effective gain = trim + volume |
| Hot-plug | Verified: unplugging removes the device cleanly (coreaudiod and the MIDI bridge keep running), plugging in re-creates it |
| UA4FX Control.app (`ui/`) | SwiftUI app: latency presets (playback fill lead, queue depths), software input/output gain + mute, live engine statistics, MIDI bridge status, coreaudiod restart |

## Build & install

```bash
make                 # -> build/UA4FX.driver, build/ua4fx_midid, build/UA4FX Control.app, tools
make install         # sudo: copies the plug-in to /Library/Audio/Plug-Ins/HAL and restarts coreaudiod;
                     # user: installs the MIDI launch agent (~/Library/LaunchAgents/com.ua4fx.midid.plist)
                     #       and UA4FX Control.app into ~/Applications
make uninstall
```

Requirements: Xcode command line tools; macOS 13+ (built and tested on macOS 27 / arm64).
The unit must be in **ADVANCED** mode (rear switch) and re-plugged after any switch change.

Check the log (use the full path: `log` is a zsh builtin):

```bash
/usr/bin/log stream --predicate 'subsystem == "com.ua4fx.driver"' --level info
```

`launchctl kickstart` of coreaudiod is refused under SIP on recent macOS, so the install
script falls back to `sudo killall coreaudiod` (launchd relaunches it immediately).

## Testing without installing

```bash
./build/ua4fx_test 10           # engine only: plays a 440 Hz tone, reads input, prints clock stats
./build/hal_harness build/UA4FX.driver 5   # drives the plug-in exactly like the HAL does
./build/rtl_test 32                        # with a loopback cable (out -> in): true analog round-trip latency
```

## How the device works (research summary)

* **Descriptors.** Device class 0xFF. Three vendor-specific interfaces with UAC1-like
  class-specific descriptors that macOS's `AppleUSBAudio` does not match:
  * if 0 (sub 2 / proto 2): alt 1 = isoc **OUT** 0x01, *adaptive*, 24-bit/3-byte stereo; alt 2 = non-PCM (IEC61937) pass-through
  * if 1 (sub 2 / proto 1): alt 1 = isoc **IN** 0x82, *asynchronous*
  * if 2 (sub 3): MIDI, alt 0 = bulk OUT 0x03 / bulk IN 0x84, alt 1 = bulk OUT / interrupt IN
* **Sample rate** is fixed by the rear switch and encoded in `wMaxPacketSize` (0x120 → 44.1k,
  0x138/0x140 → 48k, 0x258/0x260 → 96k) and in the class-specific FORMAT_TYPE descriptor.
  It cannot be set over USB. At 96 kHz the unit is half-duplex ("96 kHz REC" / "96 kHz PLAY")
  and the interface layout changes, so interfaces are classified by their endpoints, not by number.
* **No vendor requests** are needed. `SET_CONFIGURATION 1`, `SET_INTERFACE alt 1`, stream.
  This mirrors Linux `snd-usb-audio` (`create_uaxx_quirk()` in `sound/usb/quirks.c`).
* **Standard mode** (`0582:00a4`) is class compliant: 16-bit / 44.1 kHz only, no MIDI — that is what
  macOS supports out of the box. Advanced mode is what this driver adds.
* Bus-powered, 360 mA. Sources: Linux quirks table, Roland spec/manual pages, ALSA wiki
  (see the research notes in the conversation that produced this repo).

## Design notes

* **Clocking.** While capture runs the ADC frame count is the device clock. Playback packet
  sizes come from a fractional accumulator whose rate is the *measured* capture rate (frames
  per USB frame) plus a slow phase correction (tx − rx at the same bus frame, pulled to zero
  over ~2 s). The OUT endpoint is adaptive, so its PLL must see an even 48/48/…/49 pattern:
  v0.1 compared unaligned counters and produced jumpy 47/49 packets, audible as wow/flutter on
  both directions (the ADC shares the PLL). Playback-only (96 kHz PLAY) uses the nominal rate.
* **Timestamps.** `GetZeroTimeStamp` is served from the host controller's per-frame
  `frTimeStamp` values (updated at primary interrupt time). The first transfer's timestamps
  are unreliable on XHCI, so the timeline origin is fixed from the second completed transfer
  before `StartIO` returns. Reported clock algorithm: 12-point moving average.
* **Latency.** The data path is a 1 ms real-time "tick" thread (phase-locked to the USB
  frame clock) that reads capture frames straight from the transfer frame lists and writes
  playback data into already-queued DMA buffers `outputLeadMs` before transmission ("late fill").
  Transfers are queued 8 ms ahead in both directions purely as schedule headroom, so completion
  callback latency no longer matters. Safety offsets: input 2.0 ms, output lead + 1.5 ms
  (2.5 ms with the "Lowest" preset). Measured on an M-series MacBook Air at 32-frame buffers:
  0 late fills / 0 resyncs over long runs, capture frame → ring ≤ 0.2 ms, tick jitter ≤ 15 µs.
  REAPER shows `~2.2/3.2 ms` at 32 samples. The physical floor of a full-speed USB device
  (1 ms packets each way + converters) adds ~3–4 ms that no driver on any OS can remove.
  Settings persist in the HAL's plug-in storage; a geometry change goes through
  `RequestDeviceConfigurationChange`, so the host stops and restarts IO cleanly.
* **Control app protocol.** Two custom properties on the device object
  (`kAudioObjectPropertyCustomPropertyInfoList`): `'uacf'` config dict (framesPerXfer,
  xfersInFlight, inputTrimDB, outputTrimDB, inputVolumeDB, outputVolumeDB, inputMute, outputMute;
  settable; inputGainDB/outputGainDB report the effective sum) and `'uast'`
  read-only stats dict. Any CoreAudio client can use them (e.g. from a script).
* **Formats.** The HAL sees 32-bit float; conversion to/from the device's 24-bit packed PCM
  happens in `DoIOOperation`. (Exposing the native int24 physical format is a possible follow-up.)
* **Sandbox.** coreaudiod's driver host only opens IOKit user clients it has an extension for.
  `Info.plist` lists `AppleUSBHostDeviceUserClient` / `AppleUSBHostInterfaceUserClient` under
  `AudioServerPlugIn_IOKitUserClients`.
* **Hot-plug.** The plug-in is always loaded; it watches IOKit for the device and publishes /
  withdraws the CoreAudio device with `PropertiesChanged(kAudioPlugInPropertyDeviceList)`.

## License

MIT — see [LICENSE](LICENSE). Not affiliated with or endorsed by Roland Corporation.

## Layout

```
driver/   UA4FX_Driver.c  HAL plug-in       UA4FX_USB.[ch]  USB engine     Info.plist
midi/     ua4fx_midid.c   CoreMIDI bridge   com.ua4fx.midid.plist
ui/       UA4FXControl.swift  SwiftUI control app   Info.plist
tools/    ua4fx_test.c    engine exerciser  hal_harness.c   plug-in exerciser
scripts/  install.sh / uninstall.sh
```

## Known gaps / next steps

1. Test 96 kHz REC / PLAY positions and hot-plug cycles.
2. MIDI round-trip test with a real instrument.
3. Optional: native int24 physical format, device icon, box object.

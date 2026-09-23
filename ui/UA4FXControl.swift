// UA4FX Control — settings & monitoring app for the UA-4FX user-space driver.
// Talks to the HAL plug-in through two custom CoreAudio properties on the device:
//   'uacf' config (read/write CFPropertyList dict), 'uast' stats (read-only dict).
import SwiftUI
import CoreAudio
import CoreMIDI
import AppKit

let kConfigSelector: AudioObjectPropertySelector = 0x75616366 // 'uacf'
let kStatsSelector:  AudioObjectPropertySelector = 0x75617374 // 'uast'

// MARK: - CoreAudio access

struct HAL {
    static func findDevice() -> AudioObjectID? {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr else { return nil }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr else { return nil }
        for id in ids {
            var uaddr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyModelUID, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
            var s: Unmanaged<CFString>? = nil
            var sz = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
            if AudioObjectGetPropertyData(id, &uaddr, 0, nil, &sz, &s) == noErr, let str = s?.takeRetainedValue() as String?, str == "com.ua4fx.driver:UA-4FX" { return id }
        }
        return nil
    }
    static func getDict(_ dev: AudioObjectID, _ sel: AudioObjectPropertySelector) -> [String: Any]? {
        var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var pl: Unmanaged<CFPropertyList>? = nil
        var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        let st = AudioObjectGetPropertyData(dev, &addr, 0, nil, &size, &pl)
        guard st == noErr, let v = pl?.takeRetainedValue() as? [String: Any] else { return nil }
        return v
    }
    @discardableResult
    static func setConfig(_ dev: AudioObjectID, _ d: [String: Any]) -> OSStatus {
        var addr = AudioObjectPropertyAddress(mSelector: kConfigSelector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        let cf = d as CFDictionary
        var pl: CFPropertyList? = cf
        return withUnsafePointer(to: &pl) { AudioObjectSetPropertyData(dev, &addr, 0, nil, UInt32(MemoryLayout<CFPropertyList?>.size), $0) }
    }
    static func getUInt32(_ dev: AudioObjectID, _ sel: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope) -> UInt32? {
        var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
        var v: UInt32 = 0; var size = UInt32(4)
        return AudioObjectGetPropertyData(dev, &addr, 0, nil, &size, &v) == noErr ? v : nil
    }
    static func getDouble(_ dev: AudioObjectID, _ sel: AudioObjectPropertySelector) -> Double? {
        var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var v: Double = 0; var size = UInt32(8)
        return AudioObjectGetPropertyData(dev, &addr, 0, nil, &size, &v) == noErr ? v : nil
    }
    static func midiOnline() -> (Bool, Bool) {   // (endpoints exist, online)
        var found = false, online = false
        for i in 0..<MIDIGetNumberOfSources() {
            let ep = MIDIGetSource(i)
            var name: Unmanaged<CFString>? = nil
            MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &name)
            if let n = name?.takeRetainedValue() as String?, n == "UA-4FX MIDI In" {
                found = true; var off: Int32 = 1; MIDIObjectGetIntegerProperty(ep, kMIDIPropertyOffline, &off); online = off == 0
            }
        }
        return (found, online)
    }
}

// MARK: - Model

final class Model: ObservableObject {
    @Published var device: AudioObjectID? = nil
    @Published var stats: [String: Any] = [:]
    @Published var framesPerXfer = 1
    @Published var xfersInFlight = 4
    @Published var xfersInFlightOut = 8
    @Published var outputLead = 2
    @Published var inputGain = 0.0      // trim (dB), set here
    @Published var outputGain = 7.0     // trim (dB), set here
    @Published var inputVol = 0.0       // macOS volume control (dB), read back
    @Published var outputVol = 0.0
    @Published var inputMute = false
    @Published var outputMute = false
    @Published var midi: (Bool, Bool) = (false, false)
    @Published var lastError = ""
    @Published var pluginInstalled = FileManager.default.fileExists(atPath: "/Library/Audio/Plug-Ins/HAL/UA4FX.driver")
    private var timer: Timer?
    private var suppress = false

    init() { refresh(); loadConfig(); timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.refresh() } }

    func refresh() {
        let dev = HAL.findDevice()
        if dev != device { device = dev; loadConfig() }
        stats = dev.flatMap { HAL.getDict($0, kStatsSelector) } ?? [:]
        if let dev = dev, let c = HAL.getDict(dev, kConfigSelector) {   // volume/mute can change from the system side
            inputVol = c["inputVolumeDB"] as? Double ?? inputVol
            outputVol = c["outputVolumeDB"] as? Double ?? outputVol
            let im = c["inputMute"] as? Bool ?? inputMute, om = c["outputMute"] as? Bool ?? outputMute
            if im != inputMute || om != outputMute { suppress = true; inputMute = im; outputMute = om; suppress = false }
        }
        midi = HAL.midiOnline()
        pluginInstalled = FileManager.default.fileExists(atPath: "/Library/Audio/Plug-Ins/HAL/UA4FX.driver")
    }
    func loadConfig() {
        guard let dev = device, let c = HAL.getDict(dev, kConfigSelector) else { return }
        suppress = true
        framesPerXfer = c["framesPerXfer"] as? Int ?? framesPerXfer
        xfersInFlight = c["xfersInFlight"] as? Int ?? xfersInFlight
        xfersInFlightOut = c["xfersInFlightOut"] as? Int ?? xfersInFlightOut
        outputLead = c["outputLeadMs"] as? Int ?? outputLead
        inputGain = c["inputTrimDB"] as? Double ?? inputGain
        outputGain = c["outputTrimDB"] as? Double ?? outputGain
        inputVol = c["inputVolumeDB"] as? Double ?? inputVol
        outputVol = c["outputVolumeDB"] as? Double ?? outputVol
        inputMute = c["inputMute"] as? Bool ?? inputMute
        outputMute = c["outputMute"] as? Bool ?? outputMute
        suppress = false
    }
    func push(_ d: [String: Any]) {
        guard !suppress, let dev = device else { return }
        let st = HAL.setConfig(dev, d)
        lastError = st == noErr ? "" : "Set config failed: \(st)"
    }
    func applyGeometry() { push(["framesPerXfer": framesPerXfer, "xfersInFlight": xfersInFlight, "xfersInFlightOut": xfersInFlightOut, "outputLeadMs": outputLead]) }
    func applyGains() { push(["inputTrimDB": inputGain, "outputTrimDB": outputGain, "inputMute": inputMute, "outputMute": outputMute]) }

    var rate: Double { stats["sampleRate"] as? Double ?? 48000 }
    func ms(_ frames: Any?) -> String { guard let f = frames as? Int else { return "–" }; return String(format: "%d fr / %.1f ms", f, Double(f) * 1000 / rate) }
    func int(_ k: String) -> Int { (stats[k] as? Int) ?? Int((stats[k] as? Double) ?? 0) }
    func dbl(_ k: String) -> Double { (stats[k] as? Double) ?? Double(stats[k] as? Int ?? 0) }
    func bool(_ k: String) -> Bool { stats[k] as? Bool ?? false }
    // predicted safety offsets for the *selected* geometry (same formula as the engine)
    var predictedOut: Double { Double(outputLead) + 1.5 }
    var predictedIn: Double { Double(framesPerXfer) + 1.5 }
}

// MARK: - Views

struct StatusRow: View {
    let label: String; let value: String; var color: Color = .primary
    var body: some View { HStack { Text(label).foregroundColor(.secondary); Spacer(); Text(value).foregroundColor(color).monospacedDigit() } }
}

struct ContentView: View {
    @ObservedObject var m: Model
    @State private var restartMessage = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                GroupBox(label: Label("Device", systemImage: "waveform")) {
                    VStack(spacing: 6) {
                        StatusRow(label: "HAL plug-in", value: m.pluginInstalled ? "installed" : "not installed", color: m.pluginInstalled ? .green : .red)
                        StatusRow(label: "UA-4FX", value: m.device != nil ? "connected (Advanced mode)" : "not found", color: m.device != nil ? .green : .orange)
                        StatusRow(label: "Sample rate", value: m.device != nil ? String(format: "%.0f Hz (rear switch)", m.rate) : "–")
                        StatusRow(label: "IO", value: m.bool("running") ? "running, \(m.int("ioClients")) client(s)" : "idle", color: m.bool("running") ? .green : .secondary)
                        StatusRow(label: "Clock source", value: m.bool("running") ? (m.bool("captureMaster") ? "ADC (capture master)" : "USB SOF (playback only)") : "–")
                        StatusRow(label: "MIDI bridge", value: m.midi.0 ? (m.midi.1 ? "UA-4FX MIDI In/Out online" : "endpoints offline") : "not running", color: m.midi.0 && m.midi.1 ? .green : .orange)
                    }.padding(6)
                }

                GroupBox(label: Label("Latency", systemImage: "timer")) {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Text("Preset:")
                            Button("Lowest") { m.framesPerXfer = 1; m.xfersInFlight = 4; m.xfersInFlightOut = 8; m.outputLead = 1; m.applyGeometry() }
                            Button("Low (default)") { m.framesPerXfer = 1; m.xfersInFlight = 4; m.xfersInFlightOut = 8; m.outputLead = 2; m.applyGeometry() }
                            Button("Balanced") { m.framesPerXfer = 1; m.xfersInFlight = 4; m.xfersInFlightOut = 8; m.outputLead = 3; m.applyGeometry() }
                            Button("Safe") { m.framesPerXfer = 1; m.xfersInFlight = 6; m.xfersInFlightOut = 8; m.outputLead = 5; m.applyGeometry() }
                        }
                        Stepper("USB transfer size: \(m.framesPerXfer) ms", value: $m.framesPerXfer, in: 1...8, onEditingChanged: { if !$0 { m.applyGeometry() } })
                        Stepper("Capture transfers in flight: \(m.xfersInFlight)", value: $m.xfersInFlight, in: 2...8, onEditingChanged: { if !$0 { m.applyGeometry() } })
                        Stepper("Playback transfers in flight: \(m.xfersInFlightOut)", value: $m.xfersInFlightOut, in: 2...8, onEditingChanged: { if !$0 { m.applyGeometry() } })
                        Stepper("Playback fill lead: \(m.outputLead) ms", value: $m.outputLead, in: 1...7, onEditingChanged: { if !$0 { m.applyGeometry() } })
                        Divider()
                        StatusRow(label: "Output safety offset (driver)", value: m.ms(m.stats["safetyOffsetOutput"]))
                        StatusRow(label: "Input safety offset (driver)", value: m.ms(m.stats["safetyOffsetInput"]))
                        StatusRow(label: "Selected geometry → out / in", value: String(format: "%.1f ms / %.1f ms", m.predictedOut, m.predictedIn))
                        Text("Total round trip ≈ DAW buffer × 2 + out + in + converters (~1 ms). Output latency is set by the fill lead (data is written into the queued USB buffers that many ms before transmission); transfers in flight only give the USB schedule headroom. If 'Late fills' or 'Schedule resyncs' keep counting, raise the lead by 1 ms. Changing geometry restarts the device IO (apps re-open it automatically).")
                            .font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
                    }.padding(6)
                }

                GroupBox(label: Label("Levels (software, in the driver)", systemImage: "slider.horizontal.3")) {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack { Text("Input trim").frame(width: 90, alignment: .leading); Slider(value: $m.inputGain, in: -24...24, step: 0.5, onEditingChanged: { if !$0 { m.applyGains() } }); Text(String(format: "%+.1f dB", m.inputGain)).frame(width: 70).monospacedDigit(); Toggle("Mute", isOn: $m.inputMute).onChange(of: m.inputMute) { _ in m.applyGains() } }
                        HStack { Text("Output trim").frame(width: 90, alignment: .leading); Slider(value: $m.outputGain, in: -24...24, step: 0.5, onEditingChanged: { if !$0 { m.applyGains() } }); Text(String(format: "%+.1f dB", m.outputGain)).frame(width: 70).monospacedDigit(); Toggle("Mute", isOn: $m.outputMute).onChange(of: m.outputMute) { _ in m.applyGains() } }
                        HStack { Button("Reset trims (in 0 dB, out +7 dB)") { m.inputGain = 0; m.outputGain = 7; m.inputMute = false; m.outputMute = false; m.applyGains() } }
                        Divider()
                        StatusRow(label: "macOS output volume", value: String(format: "%+.1f dB  →  effective %+.1f dB", m.outputVol, m.outputVol + m.outputGain))
                        StatusRow(label: "macOS input volume", value: String(format: "%+.1f dB  →  effective %+.1f dB", m.inputVol, m.inputVol + m.inputGain))
                        Text("Trim is a fixed offset set here; the macOS volume slider / media keys move a separate 0…−60 dB control on top of it. Effective gain = trim + volume. Positive trim can clip full-scale material (hard clip at 0 dBFS). The UA-4FX's own knobs (input sens, mic gain, effects, output volume) are hardware-only and not controllable over USB.")
                            .font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
                    }.padding(6)
                }

                GroupBox(label: Label("Engine statistics", systemImage: "chart.bar")) {
                    VStack(spacing: 4) {
                        StatusRow(label: "Capture clock vs host", value: m.bool("running") ? String(format: "%.3f Hz", m.dbl("measuredRate")) : "–")
                        StatusRow(label: "Capture frames per USB frame", value: m.bool("running") ? String(format: "%.5f", m.dbl("rxPerBusFrame")) : "–")
                        StatusRow(label: "Playback rate in use", value: m.bool("running") ? String(format: "%.5f fr/ms", m.dbl("outRate")) : "–")
                        StatusRow(label: "Feedback error (tx − rx)", value: m.bool("running") ? "\(m.int("feedbackError")) frames" : "–", color: abs(m.int("feedbackError")) > 8 ? .orange : .primary)
                        StatusRow(label: "Frames rx / tx", value: String(format: "%.0f / %.0f", m.dbl("rxFrames"), m.dbl("txFrames")))
                        StatusRow(label: "USB errors rx / tx", value: String(format: "%.0f / %.0f", m.dbl("rxErrors"), m.dbl("txErrors")), color: (m.dbl("rxErrors") + m.dbl("txErrors")) > 0 ? .orange : .primary)
                        StatusRow(label: "Schedule resyncs", value: String(format: "%.0f", m.dbl("resyncs")), color: m.dbl("resyncs") > 0 ? .orange : .primary)
                        StatusRow(label: "Playback pointer snaps", value: "\(m.int("snaps"))", color: m.int("snaps") > 0 ? .orange : .primary)
                        StatusRow(label: "Tick wake-up lateness (max)", value: m.bool("running") ? String(format: "%.0f µs", m.dbl("maxCompletionLatencyUs")) : "–", color: m.dbl("maxCompletionLatencyUs") > 500 ? .orange : .primary)
                        StatusRow(label: "Late ticks (> 0.7 ms)", value: "\(m.int("lateCompletions"))", color: m.int("lateCompletions") > 0 ? .orange : .primary)
                        StatusRow(label: "Late fills / late harvests", value: "\(m.int("lateFills")) / \(m.int("lateHarvests"))", color: (m.int("lateFills") + m.int("lateHarvests")) > 0 ? .orange : .primary)
                        StatusRow(label: "Capture frame → ring (max)", value: m.bool("running") ? String(format: "%.0f µs", m.dbl("harvestLagMaxUs")) : "–", color: m.dbl("harvestLagMaxUs") > 2500 ? .orange : .primary)
                        StatusRow(label: "Harvested by poll / by callback", value: String(format: "%.0f / %.0f", m.dbl("harvestedByPoll"), m.dbl("harvestedByCallback")))
                        StatusRow(label: "USB thread real-time policy", value: m.stats.isEmpty ? "–" : (m.bool("rtPolicyOK") ? "yes" : "NO"), color: m.stats.isEmpty || m.bool("rtPolicyOK") ? .primary : .red)
                        StatusRow(label: "Non-nominal packets", value: "\(m.int("packetsAdjusted"))")
                        StatusRow(label: "Driver version", value: m.stats["driverVersion"] as? String ?? "–")
                    }.padding(6)
                }

                GroupBox(label: Label("Maintenance", systemImage: "wrench.and.screwdriver")) {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Button("Restart coreaudiod…") { restartCoreAudio() }
                            Button("Open Audio MIDI Setup") { NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Applications/Utilities/Audio MIDI Setup.app")) }
                            Button("Copy log command") { NSPasteboard.general.clearContents(); NSPasteboard.general.setString("/usr/bin/log stream --predicate 'subsystem == \"com.ua4fx.driver\"' --level info", forType: .string) }
                        }
                        if !restartMessage.isEmpty { Text(restartMessage).font(.caption).foregroundColor(.secondary) }
                        if !m.lastError.isEmpty { Text(m.lastError).font(.caption).foregroundColor(.red) }
                    }.padding(6)
                }
            }.padding(16)
        }
        .frame(minWidth: 560, minHeight: 720)
    }

    func restartCoreAudio() {
        let script = NSAppleScript(source: "do shell script \"killall coreaudiod\" with administrator privileges")
        var err: NSDictionary? = nil
        script?.executeAndReturnError(&err)
        restartMessage = err == nil ? "coreaudiod restarted; the device reappears in a second." : "Cancelled or failed: \(err?[NSAppleScript.errorMessage] ?? "")"
    }
}

@main
struct UA4FXControlApp: App {
    @StateObject private var model = Model()
    var body: some Scene {
        WindowGroup("UA4FX Control") { ContentView(m: model) }
            .windowResizability(.contentSize)
    }
}

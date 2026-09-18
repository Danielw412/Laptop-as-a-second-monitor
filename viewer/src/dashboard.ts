type Metrics = Record<string, unknown>;
const number = (v: unknown, unit = "", digits = 1) =>
  typeof v === "number" && Number.isFinite(v) ? `${v.toFixed(digits)}${unit}` : "n/a";
const text = (v: unknown) => typeof v === "string" ? v : "n/a";
export class Dashboard {
  private connection: Metrics = {};
  private receiver: Metrics = {};
  private host: Metrics = {};
  private hostAt = 0;
  private timer: ReturnType<typeof setInterval>;
  constructor(private root: HTMLElement) {
    this.timer = setInterval(() => this.render(), 1000);
    this.render();
  }
  reset() { this.connection = {}; this.receiver = {}; this.host = {}; this.hostAt = 0; this.render(); }
  update(value: unknown) {
    if (!value || typeof value !== "object") return;
    const m = value as Metrics;
    if (m.type === "connection-stats") {
      if (m.resetPeer) { this.receiver = {}; this.host = {}; }
      this.connection = m;
    }
    if (m.type === "receiver-stats") this.receiver = m;
    if (m.type === "host-stats") { this.host = m; this.hostAt = performance.now(); }
    this.render();
  }
  private render() {
    const c = this.connection, v = this.receiver;
    const h = performance.now() - this.hostAt < 3500 ? this.host : {};
    const route = typeof v.localCandidate === "string" && typeof v.remoteCandidate === "string"
      ? `${v.localCandidate} to ${v.remoteCandidate}` : "n/a";
    const direct = c.state === "connected" && v.localCandidate && v.remoteCandidate &&
      v.localCandidate !== "relay" && v.remoteCandidate !== "relay";
    const groups: [string, [string, string][]][] = [
      ["Connection", [
        ["Status", direct ? "Direct" : c.state === "failed" ? "Failed" : c.state === "connected" ? "Connected" : text(c.label ?? "Disconnected")],
        ["Setup to video", number(c.firstVideoMs, " ms", 0)],
        ["ICE", text(c.ice)], ["Route", route], ["Protocol", text(v.protocol).toUpperCase()],
        ["RTT", number(v.rttMs, " ms")],
      ]],
      ["Video", [
        ["Resolution", v.width && v.height ? `${v.width}×${v.height}` : "n/a"],
        ["Frame rate", number(v.fps, " FPS")],
        ["Received", number(typeof v.bitrate === "number" ? v.bitrate / 1e6 : null, " Mbps")],
        ["Codec", text(v.codec)], ["Packet loss", number(typeof v.loss === "number" ? v.loss * 100 : null, "%")],
        ["Jitter", number(v.jitterMs, " ms")], ["Frames dropped", number(v.framesDropped, "", 0)],
        ["Jitter buffer / decode", `${number(v.jitterBufferMs)} / ${number(v.decodeMs)} ms`],
        ["Packet to frame", number(v.processingMs, " ms")], ["Freezes", number(v.freezes, "", 0)],
        // Quantizer is pixelation itself; corrupted frames are the torn, smeared kind of glitch.
        ["Quantizer (blockiness)", number(v.qp, "", 0)], ["Corrupted frames", number(v.corrupted, "", 0)],
        ["Decoder", text(v.decoder)], ["NACK / PLI", `${number(v.nack,"",0)} / ${number(v.pli,"",0)}`],
      ]],
      ["Host pipeline", [
        ["Capture", number(h.capture_fps, " FPS")], ["Backend", text(h.capture_backend)],
        ["Encoder", text(h.encoder)], ["Encode", number(h.encode_ms_mean, " ms")],
        ["Frame wait / present to encoded", `${number(h.acquire_delay_ms_mean)} / ${number(h.source_to_encoded_ms_mean)} ms`],
        ["Encode p95 / p99", `${number(h.encode_ms_p95)} / ${number(h.encode_ms_p99)} ms`],
        ["Video path", text(h.video_path)], ["GPU", text(h.gpu)],
        ["CPU (machine)", number(h.cpu_percent, "%")], ["Queue", number(h.queue_depth, "", 0)],
      ]],
      ["Connection stages", [
        ["Signaling authenticated", number(c.signalingMs, " ms", 0)],
        ["Peer available", number(c.peerAvailableMs, " ms", 0)],
        ["SDP applied", number(c.sdpMs, " ms", 0)],
        ["ICE connected", number(c.iceMs, " ms", 0)],
        ["WebRTC ready", number(c.webrtcMs, " ms", 0)],
        ["First presented frame", number(c.firstVideoMs, " ms", 0)],
        ["ICE duration", number(c.iceDurationMs, " ms", 0)],
      ]],
    ];
    // All peer-provided strings are inserted as text, never HTML.
    const fragment = document.createDocumentFragment();
    for (const [title, rows] of groups) {
      const section = document.createElement("section"), heading = document.createElement("h2");
      heading.textContent = title; section.append(heading);
      const dl = document.createElement("dl");
      for (const [label, value] of rows) {
        const dt = document.createElement("dt"), dd = document.createElement("dd");
        dt.textContent = label; dd.textContent = value; dd.title = value; dl.append(dt, dd);
      }
      section.append(dl); fragment.append(section);
    }
    this.root.replaceChildren(fragment);
  }
}

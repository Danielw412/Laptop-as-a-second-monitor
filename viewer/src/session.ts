import ice from "../../shared/ice.json";
import {
  CODE_LIFETIME_MS,
  DIRECT_FAILURE,
  sha256Hex,
  type PairResponse,
  type Role,
  type ServerMessage,
} from "../../shared/protocol";
import { ReceiverStats } from "./telemetry";
/** How this session proves who it is. A viewer starts with a code and continues with the token it receives. */
export type Credentials =
  | { role: "viewer"; code: string }
  | { role: "viewer"; room: string; token: string }
  | { role: "host"; room: string; secret: string; code: string };
export type Paired = { room?: string; token?: string };
export const ERROR_TEXT: Record<string, string> = {
  "invalid-code": "That code was not recognized or has expired. Check the code shown in Browser Monitor and try again.",
  "rate-limit": "Too many attempts. Wait a minute, then enter the current code.",
  "viewer-occupied": "Another receiver is already connected to this Browser Monitor.",
  authentication: "This session is no longer valid. Enter the current code from Browser Monitor.",
  malformed: "The signaling server rejected a message.",
  role: "The signaling server rejected a message.",
  version: "This receiver and the signaling server speak different protocol versions.",
};
export class Session {
  private ws?: WebSocket;
  private pc?: RTCPeerConnection;
  private channel?: RTCDataChannel;
  private generation = "";
  private pending: RTCIceCandidateInit[] = [];
  private stopped = false;
  private retry = 0;
  private retryTimer?: ReturnType<typeof setTimeout>;
  private deadline?: ReturnType<typeof setTimeout>;
  private statsTimer?: ReturnType<typeof setInterval>;
  private chain = Promise.resolve();
  private started = performance.now();
  private stages: Record<string, unknown> = {};
  private iceStarted?: number;
  private room?: string;
  private token?: string;
  private connectionStats(resetPeer = false) {
    this.diagnostics({ type: "connection-stats", ...this.stages,
      state: this.pc?.connectionState ?? "new", ice: this.pc?.iceConnectionState,
      resetPeer });
  }
  private mark(name: string) {
    if (this.stages[name] === undefined) this.stages[name] = performance.now() - this.started;
    this.connectionStats();
  }
  presented() { this.mark("firstVideoMs"); }
  constructor(
    private server: string,
    private credentials: Credentials,
    private status: (s: string) => void,
    private error: (s: string) => void,
    private video: (s: MediaStream) => void,
    private diagnostics: (s: unknown) => void,
    private paired: (p: Paired) => void = () => {},
    private ended: (reason: string) => void = () => {},
    private stream?: MediaStream,
  ) {
    if ("room" in credentials) this.room = credentials.room;
    if ("token" in credentials) this.token = credentials.token;
  }
  get role(): Role {
    return this.credentials.role;
  }
  private signalingUrl(): URL {
    const url = new URL(this.server);
    if (
      url.protocol !== "https:" &&
      !(
        url.protocol === "http:" &&
        ["localhost", "127.0.0.1"].includes(url.hostname)
      )
    )
      throw Error("Use HTTPS signaling (HTTP is allowed only on localhost).");
    url.search = "";
    url.hash = "";
    return url;
  }
  start() {
    if (this.stopped) return;
    this.started = performance.now();
    this.stages = { label: "Connecting" };
    this.connectionStats(true);
    const url = this.signalingUrl();
    // A token outlives the code it was obtained with, so resuming prefers it over the code.
    if (this.room && (this.token || this.credentials.role === "host")) this.open(url, undefined);
    else if (this.credentials.role === "viewer" && "code" in this.credentials) void this.pair(url, this.credentials.code);
    else throw Error("Enter the pairing code shown in Browser Monitor.");
  }
  /** Exchanges the code for a room and a one-time ticket. The code travels in a request body, never in a URL. */
  private async pair(url: URL, code: string) {
    this.status("Checking the pairing code…");
    const attempt = this.started;
    let result: PairResponse;
    try {
      const response = await fetch(new URL("/pair", url), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ code }),
      });
      result = (await response.json()) as PairResponse;
    } catch {
      if (this.stopped || attempt !== this.started) return;
      this.error("Signaling connection failed. Check the server URL and network.");
      this.scheduleRetry();
      return;
    }
    if (this.stopped || attempt !== this.started) return;
    if ("error" in result) {
      if (result.error === "host-unavailable") {
        this.status("Browser Monitor is not running on the host yet. Waiting…");
        this.scheduleRetry();
        return;
      }
      this.error(ERROR_TEXT[result.error] ?? `Pairing failed: ${result.error}`);
      this.stop();
      this.ended(result.error);
      return;
    }
    this.room = result.room;
    this.open(url, result.ticket);
  }
  private scheduleRetry() {
    clearTimeout(this.retryTimer);
    this.retryTimer = setTimeout(() => this.start(), Math.min(10000, 500 * 2 ** Math.min(this.retry++, 5)));
  }
  private open(base: URL, ticket: string | undefined) {
    const url = new URL(base);
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    url.pathname = `/room/${this.room}`;
    const ws = (this.ws = new WebSocket(url));
    this.status("Connecting to signaling…");
    ws.onopen = () => {
      if (this.credentials.role === "host")
        ws.send(JSON.stringify({ type: "auth", version: 2, role: "host", secret: this.credentials.secret }));
      else if (ticket) ws.send(JSON.stringify({ type: "auth", version: 2, role: "viewer", ticket }));
      else ws.send(JSON.stringify({ type: "auth", version: 2, role: "viewer", token: this.token }));
    };
    ws.onmessage = (e) => {
      this.chain = this.chain
        .then(async () => {
          if (this.ws !== ws || this.stopped) return;
          await this.message(JSON.parse(e.data) as ServerMessage);
        })
        .catch(() =>
          this.error("Negotiation failed. Disconnect and reconnect to retry."),
        );
    };
    ws.onerror = () =>
      this.error(
        "Signaling connection failed. Check the server URL and network.",
      );
    ws.onclose = () => {
      if (this.ws !== ws || this.stopped) return;
      this.resetPeer();
      this.status("Signaling disconnected. Reconnecting…");
      this.scheduleRetry();
    };
  }
  stop() {
    this.stopped = true;
    clearTimeout(this.retryTimer);
    this.resetPeer();
    this.ws?.close();
    this.stream?.getTracks().forEach((t) => t.stop());
  }
  private send(message: object) {
    if (this.ws?.readyState === WebSocket.OPEN)
      this.ws.send(JSON.stringify({ ...message, generation: this.generation }));
  }
  private resetPeer() {
    clearTimeout(this.deadline);
    clearInterval(this.statsTimer);
    const old = this.pc;
    this.pc = undefined;
    old?.close();
    this.channel = undefined;
    this.pending = [];
    this.connectionStats(true);
  }
  private data(channel: RTCDataChannel) {
    this.channel = channel;
    channel.onmessage = (e) => {
      try {
        if (typeof e.data === "string" && e.data.length < 8192)
          this.diagnostics(JSON.parse(e.data));
      } catch {
        /* Ignore non-JSON diagnostics. */
      }
    };
  }
  private async message(m: ServerMessage) {
    if (m.type === "error") {
      if (m.code === "host-unavailable") {
        this.status("Browser Monitor is not running on the host yet. Waiting…");
        this.ws?.close();
        return;
      }
      if (m.code === "authentication" && this.token && "code" in this.credentials) {
        // The resume token was revoked (host restarted or disconnected us) but we still hold the code: retry with it.
        this.token = undefined;
        this.room = undefined;
        this.ws?.close();
        return;
      }
      this.error(ERROR_TEXT[m.code] ?? `Pairing failed: ${m.code}`);
      this.stop();
      this.ended(m.code);
      return;
    }
    if (m.type === "kicked") {
      this.status("Disconnected by the host.");
      this.error("");
      this.stop();
      this.ended("kicked");
      return;
    }
    if (m.type === "authenticated") {
      this.mark("signalingMs");
      this.retry = 0;
      this.error("");
      if (m.role === "viewer" && m.token) {
        this.token = m.token;
        if (m.room) this.room = m.room;
        this.paired({ room: this.room, token: m.token });
      }
      if (this.credentials.role === "host") {
        this.ws?.send(
          JSON.stringify({
            type: "codes",
            codes: [{ hash: await sha256Hex(this.credentials.code), ttlMs: CODE_LIFETIME_MS }],
          }),
        );
      }
      this.status(this.credentials.role === "host" ? "Code published. Waiting for the receiver…" : "Paired. Waiting for the host…");
      return;
    }
    if (m.type === "peer-left") {
      this.resetPeer();
      this.status("Other laptop disconnected. Waiting for it to return…");
      return;
    }
    if (m.type === "ready") {
      this.resetPeer();
      for (const key of ["peerAvailableMs", "sdpMs", "iceMs", "webrtcMs", "firstVideoMs", "iceDurationMs"])
        delete this.stages[key];
      this.iceStarted = undefined;
      this.mark("peerAvailableMs");
      this.generation = m.generation;
      const pc = (this.pc = new RTCPeerConnection({
        iceServers: ice.iceServers,
      }));
      const generation = m.generation;
      this.status("Establishing direct connection…");
      pc.oniceconnectionstatechange = () => {
        if (this.pc !== pc) return;
        if (pc.iceConnectionState === "checking" && this.iceStarted === undefined) this.iceStarted = performance.now();
        if (["connected", "completed"].includes(pc.iceConnectionState)) {
          if (this.stages.iceDurationMs === undefined && this.iceStarted !== undefined)
            this.stages.iceDurationMs = performance.now() - this.iceStarted;
          this.mark("iceMs");
        }
        this.connectionStats();
      };
      pc.onicecandidate = (e) => {
        if (this.pc === pc && e.candidate)
          this.send({
            type: "ice",
            generation,
            candidate: e.candidate.candidate,
            mid: e.candidate.sdpMid ?? "0",
          });
      };
      pc.ontrack = (e) => {
        if (this.pc !== pc) return;
        const receiver = e.receiver as RTCRtpReceiver & {
          jitterBufferTarget?: number;
        };
        if ("jitterBufferTarget" in receiver) receiver.jitterBufferTarget = 0;
        this.video(e.streams[0] ?? new MediaStream([e.track]));
      };
      pc.ondatachannel = (e) => this.data(e.channel);
      pc.onconnectionstatechange = () => {
        if (this.pc !== pc) return;
        this.status(`WebRTC ${pc.connectionState}`);
        this.connectionStats();
        if (pc.connectionState === "connected") {
          clearTimeout(this.deadline);
          this.error("");
          this.mark("webrtcMs");
        }
        if (pc.connectionState === "failed") this.reconnectDirect();
        if (pc.connectionState === "disconnected") {
          clearTimeout(this.deadline);
          this.deadline = setTimeout(
            () => this.reconnectDirect(),
            ice.connectionTimeoutMs,
          );
        }
      };
      this.deadline = setTimeout(() => {
        if (this.pc === pc && pc.connectionState !== "connected")
          this.reconnectDirect();
      }, ice.connectionTimeoutMs);
      const stats = new ReceiverStats();
      let sampling = false;
      this.statsTimer = setInterval(() => {
        if (sampling) return;
        sampling = true;
        void stats
          .sample(pc)
          .then((s) => {
            if (!s || this.pc !== pc) return;
            this.diagnostics(s.diagnostics);
            if (
              this.channel?.readyState === "open" &&
              this.channel.bufferedAmount < 4096
            )
              this.channel.send(JSON.stringify(s.telemetry));
          })
          .catch(() => {})
          .finally(() => {
            sampling = false;
          });
      }, 1000);
      if (this.credentials.role === "host") {
        if (!this.stream) throw Error("Missing test source");
        for (const track of this.stream.getVideoTracks()) {
          const sender = pc.addTrack(track, this.stream);
          const transceiver = pc
            .getTransceivers()
            .find((t) => t.sender === sender)!;
          const codecs = RTCRtpSender.getCapabilities("video")?.codecs.filter(
            (c) => c.mimeType === "video/H264",
          );
          if (codecs?.length) transceiver.setCodecPreferences(codecs);
        }
        this.data(
          pc.createDataChannel("telemetry", {
            ordered: false,
            maxRetransmits: 0,
          }),
        );
        await pc.setLocalDescription(await pc.createOffer());
        this.send({ type: "offer", sdp: pc.localDescription!.sdp });
      }
      return;
    }
    if (m.generation !== this.generation || !this.pc) return;
    if (m.type === "offer" || m.type === "answer") {
      await this.pc.setRemoteDescription({ type: m.type, sdp: m.sdp });
      this.mark("sdpMs");
      for (const c of this.pending) await this.pc.addIceCandidate(c);
      this.pending = [];
      if (m.type === "offer") {
        await this.pc.setLocalDescription(await this.pc.createAnswer());
        this.send({ type: "answer", sdp: this.pc.localDescription!.sdp });
      }
    } else if (m.type === "ice") {
      const c = { candidate: m.candidate, sdpMid: m.mid };
      if (this.pc.remoteDescription) await this.pc.addIceCandidate(c);
      else if (this.pending.length < 128) this.pending.push(c);
    }
  }
  private reconnectDirect() {
    this.error(DIRECT_FAILURE);
    this.stages.label = "Failed";
    this.diagnostics({type: "connection-stats", ...this.stages, state: "failed", ice: this.pc?.iceConnectionState});
    // Rejoining creates a fresh generation and triggers a new host offer.
    // Leave the failure visible before retrying; never substitute a relay.
    clearTimeout(this.retryTimer);
    this.retryTimer = setTimeout(() => this.ws?.close(), 3000);
  }
}

import ice from "../../shared/ice.json";
import {
  CODE_LIFETIME_MS,
  DIRECT_FAILURE,
  HEARTBEAT,
  HEARTBEAT_REPLY,
  sha256Hex,
  type PairResponse,
  type ProbeReply,
  type ReceiverEvent,
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
/**
 * Where the session is, as the page needs to know it: still exchanging the code ("pairing"), past pairing but
 * without a picture yet or with one that dropped ("busy"), or showing live video ("connected").
 */
export type SessionPhase = "pairing" | "busy" | "connected";
export interface SessionHandlers {
  status(text: string): void;
  error(text: string): void;
  video(stream: MediaStream): void;
  diagnostics(payload: unknown): void;
  phase?(phase: SessionPhase): void;
  paired?(p: Paired): void;
  ended?(reason: string): void;
  /** The host asked for the luma grid of the frame with this RTP timestamp (a quality probe). */
  probe?(rtp: number, cols?: number, rows?: number): Promise<Omit<ProbeReply, "type" | "id"> | undefined>;
}
export const ERROR_TEXT: Record<string, string> = {
  "invalid-code": "That code was not recognized or has expired. Check the code shown in Laptop Monitor and try again.",
  "rate-limit": "Too many attempts. Wait a minute, then enter the current code.",
  "viewer-occupied": "Another receiver is already connected to this Laptop Monitor.",
  authentication: "This session is no longer valid. Enter the current code from Laptop Monitor.",
  malformed: "The signaling server rejected a message.",
  role: "The signaling server rejected a message.",
  version: "This receiver and the signaling server speak different protocol versions.",
};
/** What each WebRTC connection state means to the person watching. */
const CONNECTION_TEXT: Record<RTCPeerConnectionState, string | undefined> = {
  new: undefined,
  connecting: "Connecting directly…",
  connected: "Connected",
  disconnected: "Connection interrupted. Reconnecting…",
  failed: "Direct connection failed. Retrying…",
  closed: "Connection closed",
};
/** A heartbeat every 20 s keeps an idle signaling socket from being timed out by a proxy on the way. */
const HEARTBEAT_MS = 20_000;
/** No reply to heartbeats for this long: the socket is dead even if the browser has not noticed. */
const HEARTBEAT_TIMEOUT_MS = 50_000;
/** Receiver events kept and repeated in telemetry until the host has certainly seen them. */
const EVENT_RING = 12;
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
  private heartbeatTimer?: ReturnType<typeof setInterval>;
  private lastHeard = 0;
  private features: string[] = [];
  private chain = Promise.resolve();
  private started = performance.now();
  private stages: Record<string, unknown> = {};
  private iceStarted?: number;
  private room?: string;
  private token?: string;
  private lastPhase?: SessionPhase;
  private events: ReceiverEvent[] = [];
  private eventId = 0;
  private lastCandidateError = "";
  private connectionStats(resetPeer = false) {
    this.on.diagnostics({ type: "connection-stats", ...this.stages,
      state: this.pc?.connectionState ?? "new", ice: this.pc?.iceConnectionState,
      resetPeer });
  }
  private mark(name: string) {
    if (this.stages[name] === undefined) this.stages[name] = performance.now() - this.started;
    this.connectionStats();
  }
  private phase(p: SessionPhase) {
    if (p === this.lastPhase) return;
    this.lastPhase = p;
    this.on.phase?.(p);
  }
  /** Something the host's log should know about; delivered with the next telemetry messages. */
  note(text: string) {
    this.events.push({ id: ++this.eventId + Date.now() * 1000, at: new Date().toISOString(), text: text.slice(0, 180) });
    if (this.events.length > EVENT_RING) this.events.shift();
  }
  presented() { this.mark("firstVideoMs"); }
  constructor(
    private server: string,
    private credentials: Credentials,
    private on: SessionHandlers,
    private stream?: MediaStream,
  ) {
    if ("room" in credentials) this.room = credentials.room;
    if ("token" in credentials) this.token = credentials.token;
  }
  get role(): Role {
    return this.credentials.role;
  }
  /** A working media connection: kept across signaling reconnects. */
  private live(): boolean {
    return this.pc?.connectionState === "connected";
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
    if (!this.live()) {
      this.stages = { label: "Connecting" };
      this.connectionStats(true);
    }
    const url = this.signalingUrl();
    // A token outlives the code it was obtained with, so resuming prefers it over the code.
    if (this.room && (this.token || this.credentials.role === "host")) {
      if (!this.live()) this.phase("busy");
      this.open(url, undefined);
    } else if (this.credentials.role === "viewer" && "code" in this.credentials) {
      this.phase("pairing");
      void this.pair(url, this.credentials.code);
    } else throw Error("Enter the pairing code shown in Laptop Monitor.");
  }
  /** Exchanges the code for a room and a one-time ticket. The code travels in a request body, never in a URL. */
  private async pair(url: URL, code: string) {
    this.on.status("Checking the pairing code…");
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
      this.on.error("Signaling connection failed. Check the server URL and network.");
      this.scheduleRetry();
      return;
    }
    if (this.stopped || attempt !== this.started) return;
    if ("error" in result) {
      if (result.error === "host-unavailable") {
        this.on.status("Laptop Monitor is not running on the host yet. Waiting…");
        this.scheduleRetry();
        return;
      }
      this.on.error(ERROR_TEXT[result.error] ?? `Pairing failed: ${result.error}`);
      this.stop();
      this.on.ended?.(result.error);
      return;
    }
    this.room = result.room;
    this.phase("busy");
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
    const opened = performance.now();
    this.features = [];
    if (!this.live()) this.on.status("Connecting to signaling…");
    ws.onopen = () => {
      // With a media connection still up, say so: a worker that keeps sessions then resumes it.
      const live = this.live() && this.generation ? { live: this.generation } : {};
      if (this.credentials.role === "host")
        ws.send(JSON.stringify({ type: "auth", version: 2, role: "host", secret: this.credentials.secret, ...live }));
      else if (ticket) ws.send(JSON.stringify({ type: "auth", version: 2, role: "viewer", ticket, ...live }));
      else ws.send(JSON.stringify({ type: "auth", version: 2, role: "viewer", token: this.token, ...live }));
    };
    ws.onmessage = (e) => {
      this.lastHeard = performance.now();
      if (e.data === HEARTBEAT_REPLY) return;
      this.chain = this.chain
        .then(async () => {
          if (this.ws !== ws || this.stopped) return;
          await this.message(JSON.parse(e.data) as ServerMessage);
        })
        .catch((error: unknown) => {
          this.note(`negotiation error: ${error instanceof Error ? error.message : String(error)}`);
          this.on.error("Negotiation failed. Disconnect and reconnect to retry.");
        });
    };
    ws.onerror = () => {
      if (!this.live())
        this.on.error("Signaling connection failed. Check the server URL and network.");
    };
    ws.onclose = (e) => {
      if (this.ws !== ws || this.stopped) return;
      clearInterval(this.heartbeatTimer);
      const seconds = ((performance.now() - opened) / 1000).toFixed(1);
      if (this.live()) {
        // The WebSocket only introduces the two machines; the video does not need it. Keep the picture and
        // reconnect in the background.
        this.note(`signaling closed (code ${e.code}${e.reason ? ` ${e.reason}` : ""}, after ${seconds} s); media kept`);
      } else {
        this.note(`signaling closed (code ${e.code}${e.reason ? ` ${e.reason}` : ""}, after ${seconds} s)`);
        this.resetPeer();
        this.phase("busy");
        this.on.status("Signaling disconnected. Reconnecting…");
      }
      this.scheduleRetry();
    };
  }
  /** Heartbeats only where the worker answers them (an older worker would reject the frame). */
  private startHeartbeat() {
    clearInterval(this.heartbeatTimer);
    if (!this.features.includes("heartbeat")) return;
    this.lastHeard = performance.now();
    const ws = this.ws;
    this.heartbeatTimer = setInterval(() => {
      if (this.ws !== ws || ws?.readyState !== WebSocket.OPEN) return;
      if (performance.now() - this.lastHeard > HEARTBEAT_TIMEOUT_MS) {
        this.note("signaling heartbeat unanswered; reconnecting");
        ws.close();
        return;
      }
      ws.send(HEARTBEAT);
    }, HEARTBEAT_MS);
  }
  stop() {
    this.stopped = true;
    clearTimeout(this.retryTimer);
    clearInterval(this.heartbeatTimer);
    this.resetPeer();
    this.ws?.close();
    this.stream?.getTracks().forEach((t) => t.stop());
  }
  private send(message: object) {
    if (this.ws?.readyState === WebSocket.OPEN)
      this.ws.send(JSON.stringify({ ...message, generation: this.generation }));
  }
  /** Tells a worker that keeps sessions whether this side's media connection for the generation is up. */
  private sendState(live: boolean) {
    if (this.features.includes("resume") && this.generation) this.send({ type: "state", live });
  }
  /** Sends a message to the host over the telemetry channel, if it is open. */
  sendToHost(message: object): boolean {
    if (this.channel?.readyState !== "open" || this.channel.bufferedAmount >= 16384) return false;
    this.channel.send(JSON.stringify(message));
    return true;
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
      if (typeof e.data !== "string" || e.data.length >= 8192) return;
      let m: unknown;
      try {
        m = JSON.parse(e.data);
      } catch {
        return; /* Ignore non-JSON diagnostics. */
      }
      const request = m as { type?: string; id?: unknown; rtp?: unknown; cols?: unknown; rows?: unknown };
      if (request.type === "probe-request" && typeof request.id === "number" && typeof request.rtp === "number") {
        const id = request.id;
        // The grid size comes from the host, within reason (the reply must stay a modest message).
        const size = (v: unknown, max: number) =>
          typeof v === "number" && Number.isInteger(v) && v > 0 && v <= max ? v : undefined;
        const cols = size(request.cols, 480), rows = size(request.rows, 270);
        void (this.on.probe?.(request.rtp, cols, rows) ?? Promise.resolve(undefined)).then((grid) =>
          this.sendToHost({ type: "probe", id, ...(grid ?? { missed: true }) } satisfies ProbeReply),
        );
        return;
      }
      this.on.diagnostics(m);
    };
  }
  private async message(m: ServerMessage) {
    if (m.type === "error") {
      this.note(`signaling error: ${m.code}`);
      if (m.code === "host-unavailable") {
        this.on.status("Laptop Monitor is not running on the host yet. Waiting…");
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
      this.on.error(ERROR_TEXT[m.code] ?? `Pairing failed: ${m.code}`);
      this.stop();
      this.on.ended?.(m.code);
      return;
    }
    if (m.type === "kicked") {
      this.on.status("Disconnected by the host.");
      this.on.error("");
      this.stop();
      this.on.ended?.("kicked");
      return;
    }
    if (m.type === "authenticated") {
      this.mark("signalingMs");
      this.retry = 0;
      this.on.error("");
      this.features = Array.isArray(m.features) ? m.features.filter((f) => typeof f === "string") : [];
      this.startHeartbeat();
      if (m.role === "viewer" && m.token) {
        this.token = m.token;
        if (m.room) this.room = m.room;
        this.on.paired?.({ room: this.room, token: m.token });
      }
      if (this.credentials.role === "host") {
        this.ws?.send(
          JSON.stringify({
            type: "codes",
            codes: [{ hash: await sha256Hex(this.credentials.code), ttlMs: CODE_LIFETIME_MS }],
          }),
        );
      }
      if (this.live()) this.sendState(true);
      else this.on.status(this.credentials.role === "host" ? "Code published. Waiting for the receiver…" : "Paired. Waiting for the host…");
      return;
    }
    if (m.type === "peer-left") {
      if (this.live()) {
        this.note("host's signaling connection dropped; media still up");
        return;
      }
      this.resetPeer();
      this.phase("busy");
      this.on.status("Other laptop disconnected. Waiting for it to return…");
      return;
    }
    if (m.type === "ready") {
      if (m.resume && m.generation === this.generation && this.live()) {
        this.note("signaling resumed; media connection kept");
        return;
      }
      this.generation = m.generation;
      await this.createPeer();
      return;
    }
    if (m.generation !== this.generation || !this.pc) return;
    if (m.type === "offer" || m.type === "answer") {
      if (m.type === "offer" && this.pc.remoteDescription) {
        // The host started over within the same generation (its media connection had gone): so do we.
        this.note("host renegotiated; new media connection");
        await this.createPeer();
      }
      const pc = this.pc!;
      await pc.setRemoteDescription({ type: m.type, sdp: m.sdp });
      this.mark("sdpMs");
      for (const c of this.pending) await pc.addIceCandidate(c);
      this.pending = [];
      if (m.type === "offer") {
        await pc.setLocalDescription(await pc.createAnswer());
        this.send({ type: "answer", sdp: pc.localDescription!.sdp });
      }
    } else if (m.type === "ice") {
      const c = { candidate: m.candidate, sdpMid: m.mid };
      if (this.pc.remoteDescription) await this.pc.addIceCandidate(c);
      else if (this.pending.length < 128) this.pending.push(c);
    }
  }
  /** A fresh RTCPeerConnection for the current generation (and, as the test host, the offer). */
  private async createPeer() {
    this.resetPeer();
    for (const key of ["peerAvailableMs", "sdpMs", "iceMs", "webrtcMs", "firstVideoMs", "iceDurationMs"])
      delete this.stages[key];
    this.iceStarted = undefined;
    this.mark("peerAvailableMs");
    const pc = (this.pc = new RTCPeerConnection({
      iceServers: ice.iceServers,
    }));
    const generation = this.generation;
    this.note(`negotiating (generation ${generation.slice(0, 8)})`);
    this.on.status("Establishing direct connection…");
    pc.oniceconnectionstatechange = () => {
      if (this.pc !== pc) return;
      if (pc.iceConnectionState === "checking" && this.iceStarted === undefined) this.iceStarted = performance.now();
      if (["connected", "completed"].includes(pc.iceConnectionState)) {
        if (this.stages.iceDurationMs === undefined && this.iceStarted !== undefined)
          this.stages.iceDurationMs = performance.now() - this.iceStarted;
        this.mark("iceMs");
      }
      if (["disconnected", "failed"].includes(pc.iceConnectionState)) this.note(`ICE ${pc.iceConnectionState}`);
      this.connectionStats();
    };
    pc.onicecandidateerror = (e) => {
      // Usually a STUN server that cannot be reached; logged once per kind so a blocked network is on record.
      const text = `candidate error ${e.errorCode} ${e.errorText}`;
      if (text !== this.lastCandidateError) {
        this.lastCandidateError = text;
        this.note(text);
      }
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
      this.on.video(e.streams[0] ?? new MediaStream([e.track]));
    };
    pc.ondatachannel = (e) => this.data(e.channel);
    pc.onconnectionstatechange = () => {
      if (this.pc !== pc) return;
      const text = CONNECTION_TEXT[pc.connectionState];
      if (text) this.on.status(text);
      this.connectionStats();
      this.note(`WebRTC ${pc.connectionState}`);
      if (pc.connectionState === "connected") {
        clearTimeout(this.deadline);
        this.on.error("");
        this.mark("webrtcMs");
        this.phase("connected");
        this.sendState(true);
      } else if (pc.connectionState !== "new" && pc.connectionState !== "connecting") {
        this.phase("busy");
        this.sendState(false);
        // Without media there is nothing left to keep the signaling socket optional for.
        if (this.ws?.readyState !== WebSocket.OPEN && pc.connectionState !== "disconnected") this.resetPeer();
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
      if (this.pc === pc && pc.connectionState !== "connected") this.reconnectDirect();
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
          this.on.diagnostics(s.diagnostics);
          this.sendToHost({ ...s.telemetry, events: this.events });
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
  }
  private reconnectDirect() {
    this.note(`direct connection failed (ICE ${this.pc?.iceConnectionState ?? "none"}); rejoining for a new negotiation`);
    this.on.error(DIRECT_FAILURE);
    this.stages.label = "Failed";
    this.phase("busy");
    this.on.diagnostics({type: "connection-stats", ...this.stages, state: "failed", ice: this.pc?.iceConnectionState});
    // Rejoining creates a fresh generation and triggers a new host offer.
    // Leave the failure visible before retrying; never substitute a relay.
    clearTimeout(this.retryTimer);
    this.retryTimer = setTimeout(() => {
      this.resetPeer();
      if (this.ws?.readyState === WebSocket.OPEN) this.ws.close();
      else this.start();
    }, 3000);
  }
}

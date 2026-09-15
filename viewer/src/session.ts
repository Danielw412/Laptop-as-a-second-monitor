import ice from "../../shared/ice.json";
import {
  DIRECT_FAILURE,
  type Role,
  type ServerMessage,
} from "../../shared/protocol";
import { ReceiverStats } from "./telemetry";
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
  constructor(
    private server: string,
    private room: string,
    private secret: string,
    private role: Role,
    private status: (s: string) => void,
    private error: (s: string) => void,
    private video: (s: MediaStream) => void,
    private diagnostics: (s: unknown) => void,
    private stream?: MediaStream,
  ) {}
  start() {
    if (this.stopped) return;
    const url = new URL(this.server);
    if (
      url.protocol !== "https:" &&
      !(
        url.protocol === "http:" &&
        ["localhost", "127.0.0.1"].includes(url.hostname)
      )
    )
      throw Error("Use HTTPS signaling (HTTP is allowed only on localhost).");
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    url.pathname = `/room/${this.room}`;
    url.search = "";
    url.hash = "";
    const ws = (this.ws = new WebSocket(url));
    this.status("Connecting to signaling…");
    ws.onopen = () =>
      ws.send(
        JSON.stringify({
          type: "auth",
          version: 1,
          role: this.role,
          secret: this.secret,
        }),
      );
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
      this.retryTimer = setTimeout(
        () => this.start(),
        Math.min(10000, 500 * 2 ** Math.min(this.retry++, 5)),
      );
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
  }
  private data(channel: RTCDataChannel) {
    this.channel = channel;
    channel.onmessage = (e) => {
      try {
        this.diagnostics(JSON.parse(e.data));
      } catch {
        /* Ignore non-JSON diagnostics. */
      }
    };
  }
  private async message(m: ServerMessage) {
    if (m.type === "error") {
      this.error(`Pairing failed: ${m.code}`);
      this.stop();
      return;
    }
    if (m.type === "authenticated") {
      this.retry = 0;
      this.error("");
      this.status("Paired. Waiting for the other laptop…");
      return;
    }
    if (m.type === "peer-left") {
      this.resetPeer();
      this.status("Other laptop disconnected. Waiting for it to return…");
      return;
    }
    if (m.type === "ready") {
      this.resetPeer();
      this.generation = m.generation;
      const pc = (this.pc = new RTCPeerConnection({
        iceServers: ice.iceServers,
      }));
      const generation = m.generation;
      this.status("Establishing direct connection…");
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
        if (pc.connectionState === "connected") {
          clearTimeout(this.deadline);
          this.error("");
        }
        if (pc.connectionState === "failed") this.error(DIRECT_FAILURE);
        if (pc.connectionState === "disconnected") {
          clearTimeout(this.deadline);
          this.deadline = setTimeout(
            () => this.error(DIRECT_FAILURE),
            ice.connectionTimeoutMs,
          );
        }
      };
      this.deadline = setTimeout(() => {
        if (this.pc === pc && pc.connectionState !== "connected")
          this.error(DIRECT_FAILURE);
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
      if (this.role === "host") {
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
}

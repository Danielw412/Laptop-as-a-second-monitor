export const MAX_MESSAGE = 24 * 1024;
export const PROTOCOL_VERSION = 2;
/** Pairing code alphabet: no I, O, 0 or 1. */
export const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
export const CODE_LENGTH = 6;
export const CODE_RE = /^[A-HJ-NP-Z2-9]{6}$/;
/** Host room id: first 32 hex characters of SHA-256 over the ASCII host credential. */
export const ROOM_RE = /^[a-f0-9]{32}$/;
/** Host credential: 32 random bytes as lowercase hex. Never logged or shown to users. */
export const SECRET_RE = /^[a-f0-9]{64}$/;
export const HASH_RE = /^[a-f0-9]{64}$/;
export const TOKEN_RE = /^[a-f0-9]{64}$/;
/** A negotiation generation: the UUID the worker hands out with "ready". */
export const GENERATION_RE = /^[a-f0-9-]{36}$/;
/**
 * What this worker supports beyond protocol 2, announced in "authenticated". A client uses a feature only when the
 * worker lists it, so new clients keep working against an older deployed worker (which lists none).
 *   resume    - after a WebSocket reconnect, peers whose media connection is still up keep it: the worker answers
 *               with "ready" and resume: true instead of starting a new WebRTC negotiation. Needs "state" messages.
 *   heartbeat - the text frame "ping" is answered with "pong" by the worker's runtime, without waking it.
 */
export const FEATURES = ["resume", "heartbeat"] as const;
export const HEARTBEAT = "ping";
export const HEARTBEAT_REPLY = "pong";
/** A pairing code is valid for this long after it was generated (2 minutes plus 15 seconds of overlap). */
export const CODE_LIFETIME_MS = 135_000;
export const CODE_ROTATION_MS = 120_000;
export const MAX_CODES = 2;
export const MAX_CODE_TTL_MS = 10 * 60_000;
export type Role = "host" | "viewer";
export type Relay =
  | { type: "offer" | "answer"; generation: string; sdp: string }
  | { type: "ice"; generation: string; candidate: string; mid: string };
export type CodeRegistration = { hash: string; ttlMs: number };
/** POST /pair {code} answers with a room and a one-time ticket, or an error code. */
export type PairResponse = { room: string; ticket: string } | { error: string };
/**
 * `live` on auth: the generation whose media connection this client still has up (it reconnected only its
 * WebSocket). "state" keeps the worker's view of that current while connected; it is never relayed.
 */
export type ClientMessage =
  | { type: "auth"; version: 2; role: "host"; secret: string; live?: string }
  | { type: "auth"; version: 2; role: "viewer"; ticket?: string; token?: string; live?: string }
  | { type: "codes"; codes: CodeRegistration[] }
  | { type: "kick" }
  | { type: "state"; generation: string; live: boolean }
  | Relay;
export type ServerMessage =
  | Relay
  | { type: "ready"; generation: string; resume?: boolean }
  | { type: "authenticated"; role: Role; token?: string; room?: string; features?: string[] }
  | { type: "peer-left" }
  | { type: "kicked" }
  | { type: "error"; code: string };
export const DIRECT_FAILURE =
  "Direct WebRTC connection failed.\nThis network may block peer-to-peer WebRTC traffic.\nTURN relay is not enabled.";
function object(x: unknown): x is Record<string, unknown> {
  return !!x && typeof x === "object" && !Array.isArray(x);
}
function str(x: unknown, max: number): x is string {
  return typeof x === "string" && x.length > 0 && x.length <= max;
}
/** Uppercases and strips separators so "k7m-4q2" and "K7M 4Q2" both become "K7M4Q2". */
export function normalizeCode(input: string): string {
  return input.toUpperCase().replace(/[^A-Z2-9]/g, "");
}
export function parseClient(raw: string): ClientMessage {
  if (new TextEncoder().encode(raw).byteLength > MAX_MESSAGE)
    throw Error("oversized");
  const m: unknown = JSON.parse(raw);
  if (!object(m)) throw Error("malformed");
  if (m.type === "auth") {
    if (m.version !== PROTOCOL_VERSION) throw Error("version");
    // Optional, and only ever a generation id: anything else is dropped rather than refused.
    const live = typeof m.live === "string" && GENERATION_RE.test(m.live) ? { live: m.live } : {};
    if (m.role === "host" && typeof m.secret === "string" && SECRET_RE.test(m.secret))
      return { type: "auth", version: 2, role: "host", secret: m.secret, ...live };
    if (m.role === "viewer" && m.secret === undefined) {
      if (typeof m.ticket === "string" && TOKEN_RE.test(m.ticket) && m.token === undefined)
        return { type: "auth", version: 2, role: "viewer", ticket: m.ticket, ...live };
      if (typeof m.token === "string" && TOKEN_RE.test(m.token) && m.ticket === undefined)
        return { type: "auth", version: 2, role: "viewer", token: m.token, ...live };
    }
    throw Error("malformed");
  }
  if (m.type === "codes") {
    if (!Array.isArray(m.codes) || m.codes.length > MAX_CODES) throw Error("malformed");
    const codes: CodeRegistration[] = [];
    for (const c of m.codes) {
      if (
        !object(c) ||
        typeof c.hash !== "string" ||
        !HASH_RE.test(c.hash) ||
        typeof c.ttlMs !== "number" ||
        !Number.isInteger(c.ttlMs) ||
        c.ttlMs <= 0 ||
        c.ttlMs > MAX_CODE_TTL_MS
      )
        throw Error("malformed");
      codes.push({ hash: c.hash, ttlMs: c.ttlMs });
    }
    return { type: "codes", codes };
  }
  if (m.type === "kick") return { type: "kick" };
  if (!str(m.generation, 36) || !GENERATION_RE.test(m.generation))
    throw Error("generation");
  if (m.type === "state" && typeof m.live === "boolean")
    return { type: "state", generation: m.generation, live: m.live };
  if (
    (m.type === "offer" || m.type === "answer") &&
    str(m.sdp, 20000) &&
    /^v=0\r?\n/.test(m.sdp)
  )
    return { type: m.type, generation: m.generation, sdp: m.sdp };
  if (
    m.type === "ice" &&
    str(m.candidate, 2048) &&
    m.candidate.startsWith("candidate:") &&
    str(m.mid, 64)
  )
    return {
      type: "ice",
      generation: m.generation,
      candidate: m.candidate,
      mid: m.mid,
    };
  throw Error("malformed");
}
export function hex(bytes: ArrayBuffer | Uint8Array): string {
  return Array.from(new Uint8Array(bytes), (b) => b.toString(16).padStart(2, "0")).join("");
}
export async function sha256Hex(text: string): Promise<string> {
  return hex(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)));
}
/** The room a host credential belongs to. Verifiable by the server without stored state. */
export async function roomForSecret(secret: string): Promise<string> {
  return (await sha256Hex(secret)).slice(0, 32);
}
export interface Telemetry {
  type: "telemetry";
  loss: number | null;
  rttMs: number | null;
  jitterMs: number | null;
  bitrate: number | null;
  fps: number | null;
  dropped: number | null;
  decoded: number | null;
  /** Receiver-side delays over the last interval (ms per frame); absent on older receivers. */
  jitterBufferMs?: number | null;
  decodeMs?: number | null;
  processingMs?: number | null;
  freezes?: number | null;
  /**
   * What the picture actually looked like, for the host's log. These separate the two ways a stream goes bad:
   * `qp` is the encoder's quantizer as the decoder saw it, which is pixelation itself (roughly 20-30 normal,
   * over ~36 visibly blocky); `corrupted`, `pli` and `nack` are loss, which shows up as smearing and torn
   * blocks until a keyframe arrives. Absent on older receivers.
   */
  qp?: number | null;
  corrupted?: number | null;
  pli?: number | null;
  nack?: number | null;
  keyFramesDecoded?: number | null;
  /**
   * Frame and packet flow over the last interval (the `interval` prefix marks them apart from the cumulative
   * getStats counters of the same names), FIR requests, and how long the picture was frozen or paused (ms). A pause
   * is the browser's name for no frame for 5 s or more, which a still desktop produces. `intervalMs` is how long
   * that interval was; divide by it before comparing counts. Absent on older receivers.
   */
  intervalMs?: number | null;
  intervalFramesReceived?: number | null;
  intervalFramesDecoded?: number | null;
  intervalFramesRendered?: number | null;
  intervalPacketsReceived?: number | null;
  intervalPacketsLost?: number | null;
  fir?: number | null;
  freezeMs?: number | null;
  pauses?: number | null;
  pauseMs?: number | null;
  /** How evenly frames were shown: mean and standard deviation of the gap between them over the interval (ms). */
  interFrameDelayMs?: number | null;
  interFrameDelayStdMs?: number | null;
  /** The jitter buffer's target delay per frame over the interval (ms); near 0 while playout delay 0/0 holds. */
  jitterBufferTargetMs?: number | null;
  /** The selected candidate pair's incoming bandwidth estimate (bps), when the browser provides one. */
  availableIncomingBitrate?: number | null;
  /** The decoder in use and whether the browser calls it power efficient (hardware); often withheld. */
  decoder?: string | null;
  powerEfficientDecoder?: boolean | null;
  /**
   * What happened at the receiver (connection and ICE states, WebSocket closes with their codes, the tab being
   * hidden, the network going away), newest last. Repeated in every message until it ages out, so an event that
   * happened while the data channel was down still reaches the host's log afterwards; `id` lets it log each once.
   */
  events?: ReceiverEvent[];
}
export interface ReceiverEvent {
  id: number;
  /** The receiver's wall clock, ISO 8601. */
  at: string;
  text: string;
}
/** Host to receiver over the telemetry channel: report the luma grid (cols x rows cells) of the frame with this RTP
 * timestamp. */
export interface ProbeRequest {
  type: "probe-request";
  id: number;
  rtp: number;
  cols?: number;
  rows?: number;
}
/**
 * Receiver to host: the displayed frame's luma grid (cols x rows cells, row-major, one byte per cell, base64), or
 * `missed` when that frame was never shown. The host compares it with the grid of the surface it encoded.
 */
export interface ProbeReply {
  type: "probe";
  id: number;
  rtp?: number;
  cols?: number;
  rows?: number;
  mean?: string;
  detail?: string;
  missed?: boolean;
}
/** Receiver to host: the person watching flagged a damaged picture (the M key on the stage). */
export interface MarkMessage {
  type: "mark";
  rtp: number | null;
}

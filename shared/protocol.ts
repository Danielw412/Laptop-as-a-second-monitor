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
export type ClientMessage =
  | { type: "auth"; version: 2; role: "host"; secret: string }
  | { type: "auth"; version: 2; role: "viewer"; ticket?: string; token?: string }
  | { type: "codes"; codes: CodeRegistration[] }
  | { type: "kick" }
  | Relay;
export type ServerMessage =
  | Relay
  | { type: "ready"; generation: string }
  | { type: "authenticated"; role: Role; token?: string; room?: string }
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
    if (m.role === "host" && typeof m.secret === "string" && SECRET_RE.test(m.secret))
      return { type: "auth", version: 2, role: "host", secret: m.secret };
    if (m.role === "viewer" && m.secret === undefined) {
      if (typeof m.ticket === "string" && TOKEN_RE.test(m.ticket) && m.token === undefined)
        return { type: "auth", version: 2, role: "viewer", ticket: m.ticket };
      if (typeof m.token === "string" && TOKEN_RE.test(m.token) && m.ticket === undefined)
        return { type: "auth", version: 2, role: "viewer", token: m.token };
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
  if (!str(m.generation, 36) || !/^[a-f0-9-]{36}$/.test(m.generation))
    throw Error("generation");
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
}

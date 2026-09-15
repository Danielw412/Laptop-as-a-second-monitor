export const MAX_MESSAGE = 24 * 1024;
export const ROOM_RE = /^[A-HJ-NP-Z2-9]{8}$/;
export const SECRET_RE = /^[a-f0-9]{64}$/;
export type Role = "host" | "viewer";
export type Relay =
  | { type: "offer" | "answer"; generation: string; sdp: string }
  | { type: "ice"; generation: string; candidate: string; mid: string };
export type ClientMessage =
  { type: "auth"; version: 1; role: Role; secret: string } | Relay;
export type ServerMessage =
  | Relay
  | { type: "ready"; generation: string }
  | { type: "authenticated" }
  | { type: "peer-left" }
  | { type: "error"; code: string };
export const DIRECT_FAILURE =
  "Direct WebRTC connection failed.\nThis network may block peer-to-peer WebRTC traffic.\nTURN relay is not enabled.";
function object(x: unknown): x is Record<string, unknown> {
  return !!x && typeof x === "object" && !Array.isArray(x);
}
function str(x: unknown, max: number): x is string {
  return typeof x === "string" && x.length > 0 && x.length <= max;
}
export function parseClient(raw: string): ClientMessage {
  if (new TextEncoder().encode(raw).byteLength > MAX_MESSAGE)
    throw Error("oversized");
  const m: unknown = JSON.parse(raw);
  if (!object(m)) throw Error("malformed");
  if (
    m.type === "auth" &&
    m.version === 1 &&
    (m.role === "host" || m.role === "viewer") &&
    typeof m.secret === "string" &&
    SECRET_RE.test(m.secret)
  ) {
    return { type: "auth", version: 1, role: m.role, secret: m.secret };
  }
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

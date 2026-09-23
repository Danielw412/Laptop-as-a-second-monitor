import { DurableObject } from "cloudflare:workers";
import {
  CODE_RE,
  FEATURES,
  HEARTBEAT,
  HEARTBEAT_REPLY,
  MAX_MESSAGE,
  ROOM_RE,
  normalizeCode,
  parseClient,
  roomForSecret,
  sha256Hex,
  type PairResponse,
  type Role,
} from "../../shared/protocol";
type Attachment = {
  role?: Role;
  authenticated: boolean;
  deadline: number;
  window: number;
  count: number;
  /** The generation this peer still has a working media connection for, as it last said (auth or "state"). */
  live?: string;
};
type StoredCode = { hash: string; expires: number };
type Ticket = { hash: string; expires: number };
type ViewerToken = { hash: string; expires: number };
const TOKEN_TTL = 12 * 60 * 60 * 1000;
const TICKET_TTL = 30_000;
const MAX_TICKETS = 4;
const AUTH_DEADLINE = 10_000;
const MAX_PAIR_FAILURES = 10;
function timingSafeEqualHex(a: string, b: string): boolean {
  if (a.length !== b.length || a.length === 0 || a.length % 2) return false;
  const bytes = (s: string) => Uint8Array.from(s.match(/../g)!, (h) => parseInt(h, 16));
  return crypto.subtle.timingSafeEqual(bytes(a), bytes(b));
}
function randomHex(bytes: number): string {
  return Array.from(crypto.getRandomValues(new Uint8Array(bytes)), (b) => b.toString(16).padStart(2, "0")).join("");
}
/** Directory entry for one pairing code: which room it opens and until when. Named by SHA-256(code). */
export class Code extends DurableObject<Env> {
  async register(room: string, ttlMs: number): Promise<void> {
    const expires = Date.now() + ttlMs;
    await this.ctx.storage.put("entry", { room, expires });
    await this.ctx.storage.setAlarm(expires);
  }
  async lookup(): Promise<string | null> {
    const entry = await this.ctx.storage.get<{ room: string; expires: number }>("entry");
    if (!entry || entry.expires <= Date.now()) return null;
    return entry.room;
  }
  async alarm() {
    const entry = await this.ctx.storage.get<{ room: string; expires: number }>("entry");
    if (!entry || entry.expires <= Date.now()) await this.ctx.storage.deleteAll();
    else await this.ctx.storage.setAlarm(entry.expires);
  }
}
export class Room extends DurableObject<Env> {
  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    // Heartbeats are answered by the runtime itself: they keep idle connections (a receiver's socket carries
    // nothing once video flows) from being timed out by proxies, without waking the object or counting as messages.
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair(HEARTBEAT, HEARTBEAT_REPLY));
  }
  async fetch(): Promise<Response> {
    // Bound unauthenticated sockets too; an alarm clears abandoned handshakes.
    if (this.ctx.getWebSockets().length >= 6)
      return new Response("Room busy", { status: 429 });
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1]);
    pair[1].serializeAttachment({
      authenticated: false,
      deadline: Date.now() + AUTH_DEADLINE,
      window: Date.now(),
      count: 0,
    } satisfies Attachment);
    const alarm = await this.ctx.storage.getAlarm();
    if (alarm === null || alarm > Date.now() + AUTH_DEADLINE)
      await this.ctx.storage.setAlarm(Date.now() + AUTH_DEADLINE);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }
  /**
   * Exchanges a valid code (already resolved to this room by the worker) for a one-time ticket the viewer presents
   * on its WebSocket. Returns an error code instead when pairing is not possible right now.
   */
  async claim(codeHash: string): Promise<PairResponse> {
    if (await this.throttled()) return { error: "rate-limit" };
    if (!(await this.codeValid(codeHash))) {
      const throttle = await this.pairingFailure();
      return { error: throttle ? "rate-limit" : "invalid-code" };
    }
    if (!this.peers("host").length) return { error: "host-unavailable" };
    if (this.peers("viewer").length) return { error: "viewer-occupied" };
    const ticket = randomHex(32);
    const tickets = ((await this.ctx.storage.get<Ticket[]>("tickets")) ?? []).filter((t) => t.expires > Date.now());
    tickets.push({ hash: await sha256Hex(ticket), expires: Date.now() + TICKET_TTL });
    while (tickets.length > MAX_TICKETS) tickets.shift();
    await this.ctx.storage.put("tickets", tickets);
    return { room: this.room(), ticket };
  }
  private peers(role?: Role) {
    return this.ctx.getWebSockets().filter((w) => {
      const a = w.deserializeAttachment() as Attachment;
      return a.authenticated && (!role || a.role === role);
    });
  }
  private fail(ws: WebSocket, code: string) {
    ws.send(JSON.stringify({ type: "error", code }));
    ws.close(1008, code);
  }
  private room(): string {
    return this.ctx.id.name ?? "";
  }
  private async activeCodes(): Promise<StoredCode[]> {
    const codes = (await this.ctx.storage.get<StoredCode[]>("codes")) ?? [];
    return codes.filter((c) => c.expires > Date.now());
  }
  private async codeValid(hash: string): Promise<boolean> {
    let ok = false;
    for (const c of await this.activeCodes()) if (timingSafeEqualHex(c.hash, hash)) ok = true;
    return ok;
  }
  private async consumeTicket(ticket: string): Promise<boolean> {
    const tickets = ((await this.ctx.storage.get<Ticket[]>("tickets")) ?? []).filter((t) => t.expires > Date.now());
    const hash = await sha256Hex(ticket);
    const index = tickets.findIndex((t) => timingSafeEqualHex(t.hash, hash));
    if (index < 0) {
      await this.ctx.storage.put("tickets", tickets);
      return false;
    }
    tickets.splice(index, 1);
    await this.ctx.storage.put("tickets", tickets);
    return true;
  }
  private async tokenValid(token: string): Promise<boolean> {
    const stored = await this.ctx.storage.get<ViewerToken>("viewerToken");
    if (!stored || stored.expires <= Date.now()) return false;
    return timingSafeEqualHex(stored.hash, await sha256Hex(token));
  }
  private async pairingFailure(): Promise<boolean> {
    // Per-room throttle: after too many wrong codes, tickets or tokens, pause pairing for one minute.
    const failures = (await this.ctx.storage.get<{ count: number; window: number }>("failures")) ?? {
      count: 0,
      window: Date.now(),
    };
    if (Date.now() - failures.window > 60_000) {
      failures.window = Date.now();
      failures.count = 0;
    }
    failures.count += 1;
    await this.ctx.storage.put("failures", failures);
    return failures.count > MAX_PAIR_FAILURES;
  }
  private async throttled(): Promise<boolean> {
    const failures = await this.ctx.storage.get<{ count: number; window: number }>("failures");
    return !!failures && Date.now() - failures.window <= 60_000 && failures.count > MAX_PAIR_FAILURES;
  }
  private async startGeneration() {
    const hosts = this.peers("host"), viewers = this.peers("viewer");
    if (hosts.length !== 1 || viewers.length !== 1) return;
    // Both peers came back to a generation whose media connection they both still have: a WebSocket dropped, the
    // video did not. Let them keep it rather than tear down a working connection to negotiate a new one.
    const current = await this.ctx.storage.get<string>("generation");
    const live = (w: WebSocket) => (w.deserializeAttachment() as Attachment).live;
    if (current && live(hosts[0]) === current && live(viewers[0]) === current) {
      for (const peer of this.peers()) peer.send(JSON.stringify({ type: "ready", generation: current, resume: true }));
      return;
    }
    const generation = crypto.randomUUID();
    await this.ctx.storage.put("generation", generation);
    for (const peer of this.peers()) peer.send(JSON.stringify({ type: "ready", generation }));
  }
  async webSocketMessage(ws: WebSocket, raw: string | ArrayBuffer) {
    const a = ws.deserializeAttachment() as Attachment;
    if (typeof raw !== "string" || raw.length > MAX_MESSAGE) {
      this.fail(ws, "malformed");
      return;
    }
    if (Date.now() - a.window > 10000) {
      a.window = Date.now();
      a.count = 0;
    }
    if (++a.count > 120) {
      this.fail(ws, "rate-limit");
      return;
    }
    ws.serializeAttachment(a);
    let m;
    try {
      m = parseClient(raw);
    } catch {
      this.fail(ws, "malformed");
      return;
    }
    if (!a.authenticated) {
      if (m.type !== "auth" || Date.now() > a.deadline) {
        this.fail(ws, "authentication");
        return;
      }
      if (m.role === "host") {
        // Stateless verification: the room name must be derived from the presented credential.
        if (!timingSafeEqualHex(await roomForSecret(m.secret), this.room())) {
          this.fail(ws, "authentication");
          return;
        }
        // A restarted host replaces a stale one; the credential proves it is the same installation.
        for (const old of this.peers("host")) {
          const oa = old.deserializeAttachment() as Attachment;
          oa.authenticated = false;
          old.serializeAttachment(oa);
          old.close(1000, "replaced");
        }
      } else {
        if (await this.throttled()) {
          this.fail(ws, "rate-limit");
          return;
        }
        if (!this.peers("host").length) {
          this.fail(ws, "host-unavailable");
          return;
        }
        let ok = false;
        if (m.ticket) ok = await this.consumeTicket(m.ticket);
        else if (m.token) ok = await this.tokenValid(m.token);
        if (!ok) {
          const throttle = await this.pairingFailure();
          this.fail(ws, throttle ? "rate-limit" : "authentication");
          return;
        }
        if (this.peers("viewer").length) {
          this.fail(ws, "viewer-occupied");
          return;
        }
      }
      a.authenticated = true;
      a.role = m.role;
      a.live = m.live;
      ws.serializeAttachment(a);
      if (m.role === "viewer") {
        const token = randomHex(32);
        await this.ctx.storage.put("viewerToken", {
          hash: await sha256Hex(token),
          expires: Date.now() + TOKEN_TTL,
        } satisfies ViewerToken);
        ws.send(JSON.stringify({ type: "authenticated", role: "viewer", token, room: this.room(), features: FEATURES }));
      } else ws.send(JSON.stringify({ type: "authenticated", role: "host", features: FEATURES }));
      await this.startGeneration();
      return;
    }
    if (m.type === "auth") {
      this.fail(ws, "role");
      return;
    }
    if (m.type === "codes") {
      if (a.role !== "host") {
        this.fail(ws, "role");
        return;
      }
      const codes: StoredCode[] = m.codes.map((c) => ({ hash: c.hash, expires: Date.now() + c.ttlMs }));
      await this.ctx.storage.put("codes", codes);
      const room = this.room();
      await Promise.all(m.codes.map((c) => this.env.CODES.getByName(c.hash).register(room, c.ttlMs)));
      return;
    }
    if (m.type === "state") {
      // Only about the current generation; never relayed.
      if (m.generation === (await this.ctx.storage.get("generation"))) {
        a.live = m.live ? m.generation : undefined;
        ws.serializeAttachment(a);
      }
      return;
    }
    if (m.type === "kick") {
      if (a.role !== "host") {
        this.fail(ws, "role");
        return;
      }
      await this.ctx.storage.delete("viewerToken");
      await this.ctx.storage.delete("tickets");
      await this.ctx.storage.delete("generation");
      for (const viewer of this.peers("viewer")) {
        const va = viewer.deserializeAttachment() as Attachment;
        va.authenticated = false;
        viewer.serializeAttachment(va);
        viewer.send(JSON.stringify({ type: "kicked" }));
        viewer.close(1000, "kicked");
      }
      return;
    }
    if ((m.type === "offer" && a.role !== "host") || (m.type === "answer" && a.role !== "viewer")) {
      this.fail(ws, "role");
      return;
    }
    if (m.generation !== (await this.ctx.storage.get("generation"))) return;
    for (const peer of this.peers(a.role === "host" ? "viewer" : "host"))
      peer.send(JSON.stringify(m));
  }
  async webSocketClose(ws: WebSocket, code: number, reason: string, wasClean: boolean) {
    // Visible in `wrangler tail`: which side went and how, to put next to the host's and receiver's own logs.
    const a = ws.deserializeAttachment() as Attachment;
    console.log(JSON.stringify({ event: "close", role: a.role ?? "unauthenticated", code, reason, wasClean }));
    await this.depart(ws);
  }
  async webSocketError(ws: WebSocket, error: unknown) {
    const a = ws.deserializeAttachment() as Attachment;
    console.log(JSON.stringify({ event: "error", role: a.role ?? "unauthenticated", error: String(error) }));
    ws.close(1011, "connection");
    await this.depart(ws);
  }
  private async depart(ws: WebSocket) {
    const a = ws.deserializeAttachment() as Attachment;
    if (!a.authenticated) return;
    a.authenticated = false;
    ws.serializeAttachment(a);
    // The generation stays: if both peers come back still holding its media connection, they resume it
    // (startGeneration). A peer that comes back without one gets a fresh generation there.
    for (const peer of this.peers())
      peer.send(JSON.stringify({ type: "peer-left" }));
  }
  async alarm() {
    for (const ws of this.ctx.getWebSockets()) {
      const a = ws.deserializeAttachment() as Attachment;
      if (!a.authenticated && Date.now() >= a.deadline)
        ws.close(1008, "authentication-timeout");
    }
    const codes = await this.activeCodes();
    if (codes.length) await this.ctx.storage.put("codes", codes);
    else await this.ctx.storage.delete("codes");
    const tickets = ((await this.ctx.storage.get<Ticket[]>("tickets")) ?? []).filter((t) => t.expires > Date.now());
    if (tickets.length) await this.ctx.storage.put("tickets", tickets);
    else await this.ctx.storage.delete("tickets");
    const token = await this.ctx.storage.get<ViewerToken>("viewerToken");
    if (token && token.expires <= Date.now()) await this.ctx.storage.delete("viewerToken");
    const deadlines = this.ctx
      .getWebSockets()
      .map((w) => w.deserializeAttachment() as Attachment)
      .filter((a) => !a.authenticated && a.deadline > Date.now())
      .map((a) => a.deadline);
    for (const c of codes) deadlines.push(c.expires);
    for (const t of tickets) deadlines.push(t.expires);
    if (token && token.expires > Date.now()) deadlines.push(token.expires);
    if (deadlines.length) await this.ctx.storage.setAlarm(Math.min(...deadlines));
    else if (!this.ctx.getWebSockets().length) await this.ctx.storage.deleteAll();
  }
}
function cors(request: Request, env: Env, headers: HeadersInit = {}): Headers {
  const h = new Headers(headers);
  const origin = request.headers.get("Origin");
  if (origin && env.ALLOWED_ORIGINS.split(",").includes(origin)) {
    h.set("Access-Control-Allow-Origin", origin);
    h.set("Vary", "Origin");
  }
  return h;
}
function json(request: Request, env: Env, body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: cors(request, env, { "Content-Type": "application/json", "Cache-Control": "no-store" }),
  });
}
export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname === "/health")
      return Response.json({
        service: "Laptop Monitor signaling",
        version: 2,
        mediaRelay: false,
      });
    const origin = request.headers.get("Origin");
    if (origin && !env.ALLOWED_ORIGINS.split(",").includes(origin))
      return new Response("Origin denied", { status: 403 });
    if (url.pathname === "/pair") {
      if (request.method === "OPTIONS")
        return new Response(null, {
          status: 204,
          headers: cors(request, env, {
            "Access-Control-Allow-Methods": "POST",
            "Access-Control-Allow-Headers": "Content-Type",
            "Access-Control-Max-Age": "600",
          }),
        });
      if (request.method !== "POST") return new Response("Method not allowed", { status: 405 });
      const ip = request.headers.get("CF-Connecting-IP") ?? "unknown";
      const { success } = await env.PAIR_LIMIT.limit({ key: ip });
      if (!success) return json(request, env, { error: "rate-limit" }, 429);
      let code = "";
      try {
        const body: unknown = await request.json();
        if (body && typeof body === "object" && typeof (body as { code?: unknown }).code === "string")
          code = normalizeCode((body as { code: string }).code);
      } catch {
        /* handled below */
      }
      if (!CODE_RE.test(code)) return json(request, env, { error: "invalid-code" }, 404);
      const hash = await sha256Hex(code);
      const room = await env.CODES.getByName(hash).lookup();
      if (!room) return json(request, env, { error: "invalid-code" }, 404);
      const result = await env.ROOMS.getByName(room).claim(hash);
      if ("error" in result)
        return json(request, env, result, result.error === "rate-limit" ? 429 : result.error === "invalid-code" ? 404 : 409);
      return json(request, env, result);
    }
    const match = /^\/room\/([^/]+)$/.exec(url.pathname);
    if (request.method !== "GET" || !match || !ROOM_RE.test(match[1]) || url.search)
      return new Response("Not found", { status: 404 });
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
      return new Response("WebSocket required", { status: 426 });
    return env.ROOMS.getByName(match[1]).fetch(request);
  },
} satisfies ExportedHandler<Env>;

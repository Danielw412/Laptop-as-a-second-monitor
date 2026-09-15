import { DurableObject } from "cloudflare:workers";
import {
  MAX_MESSAGE,
  ROOM_RE,
  parseClient,
  type Role,
} from "../../shared/protocol";
type Attachment = {
  role?: Role;
  authenticated: boolean;
  deadline: number;
  window: number;
  count: number;
};
type Session = { hash: ArrayBuffer; expires: number };
const TTL = 12 * 60 * 60 * 1000;
export class Room extends DurableObject<Env> {
  async fetch(): Promise<Response> {
    // Bound unauthenticated sockets too; an alarm clears abandoned handshakes.
    if (this.ctx.getWebSockets().length >= 6)
      return new Response("Room busy", { status: 429 });
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1]);
    pair[1].serializeAttachment({
      authenticated: false,
      deadline: Date.now() + 10000,
      window: Date.now(),
      count: 0,
    } satisfies Attachment);
    const alarm = await this.ctx.storage.getAlarm();
    if (alarm === null || alarm > Date.now() + 10000)
      await this.ctx.storage.setAlarm(Date.now() + 10000);
    return new Response(null, { status: 101, webSocket: pair[0] });
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
      const hash = await crypto.subtle.digest(
        "SHA-256",
        new TextEncoder().encode(m.secret),
      );
      // Storage operations participate in DO input/output gates. No external I/O in this transition.
      const role = m.role;
      const session = await this.ctx.storage.transaction(async (tx) => {
        let stored = await tx.get<Session>("session");
        if (!stored && role === "host") {
          stored = { hash, expires: Date.now() + TTL };
          await tx.put("session", stored);
        }
        return stored;
      });
      if (!session) {
        this.fail(ws, "host-unavailable");
        return;
      }
      if (session.expires < Date.now()) {
        this.fail(ws, "expired");
        return;
      }
      if (!crypto.subtle.timingSafeEqual(session.hash, hash)) {
        this.fail(ws, "authentication");
        return;
      }
      if (this.peers(m.role).length) {
        this.fail(ws, "role-occupied");
        return;
      }
      a.authenticated = true;
      a.role = m.role;
      ws.serializeAttachment(a);
      ws.send(JSON.stringify({ type: "authenticated" }));
      if (this.peers().length === 2) {
        const generation = crypto.randomUUID();
        await this.ctx.storage.put("generation", generation);
        for (const peer of this.peers())
          peer.send(JSON.stringify({ type: "ready", generation }));
      }
      return;
    }
    if (
      m.type === "auth" ||
      (m.type === "offer" && a.role !== "host") ||
      (m.type === "answer" && a.role !== "viewer")
    ) {
      this.fail(ws, "role");
      return;
    }
    if (m.generation !== (await this.ctx.storage.get("generation"))) return;
    for (const peer of this.peers(a.role === "host" ? "viewer" : "host"))
      peer.send(JSON.stringify(m));
  }
  async webSocketClose(ws: WebSocket) {
    await this.depart(ws);
  }
  async webSocketError(ws: WebSocket) {
    ws.close(1011, "connection");
    await this.depart(ws);
  }
  private async depart(ws: WebSocket) {
    const a = ws.deserializeAttachment() as Attachment;
    if (!a.authenticated) return;
    a.authenticated = false;
    ws.serializeAttachment(a);
    await this.ctx.storage.delete("generation");
    for (const peer of this.peers())
      peer.send(JSON.stringify({ type: "peer-left" }));
  }
  async alarm() {
    for (const ws of this.ctx.getWebSockets()) {
      const a = ws.deserializeAttachment() as Attachment;
      if (!a.authenticated && Date.now() >= a.deadline)
        ws.close(1008, "authentication-timeout");
    }
    const session = await this.ctx.storage.get<Session>("session");
    if (session && session.expires <= Date.now()) {
      // Keep a daily-use room alive while its authenticated host remains connected.
      // This alarm runs twice a day; no polling loop prevents hibernation.
      if (this.peers("host").length) {
        session.expires = Date.now() + TTL;
        await this.ctx.storage.put("session", session);
        await this.ctx.storage.setAlarm(session.expires);
        return;
      }
      for (const ws of this.ctx.getWebSockets()) ws.close(1008, "expired");
      await this.ctx.storage.deleteAll();
      return;
    }
    const deadlines = this.ctx
      .getWebSockets()
      .map((w) => w.deserializeAttachment() as Attachment)
      .filter((a) => !a.authenticated && a.deadline > Date.now())
      .map((a) => a.deadline);
    if (session) deadlines.push(session.expires);
    if (deadlines.length)
      await this.ctx.storage.setAlarm(Math.min(...deadlines));
  }
}
export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname === "/health")
      return Response.json({
        service: "Browser Monitor signaling",
        version: 1,
        mediaRelay: false,
      });
    const match = /^\/room\/([^/]+)$/.exec(url.pathname);
    if (
      request.method !== "GET" ||
      !match ||
      !ROOM_RE.test(match[1]) ||
      url.search
    )
      return new Response("Not found", { status: 404 });
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
      return new Response("WebSocket required", { status: 426 });
    const origin = request.headers.get("Origin");
    if (origin && !env.ALLOWED_ORIGINS.split(",").includes(origin))
      return new Response("Origin denied", { status: 403 });
    return env.ROOMS.getByName(match[1]).fetch(request);
  },
} satisfies ExportedHandler<Env>;

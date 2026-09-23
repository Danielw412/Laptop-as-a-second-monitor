// End-to-end check of the v2 pairing protocol against a running signaling worker (default: wrangler dev).
import WebSocket from "ws";
import assert from "node:assert/strict";
import { createHash, randomBytes } from "node:crypto";
const base = process.env.SIGNALING_URL ?? "ws://127.0.0.1:8787";
const http = base.replace(/^ws/, "http");
const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const sha256 = (text) => createHash("sha256").update(text).digest("hex");
const secret = randomBytes(32).toString("hex");
const room = sha256(secret).slice(0, 32);
const newCode = () => Array.from(randomBytes(6), (b) => alphabet[b % 32]).join("");
const sockets = [];
async function connect(path) {
  const ws = new WebSocket(`${base}${path}`);
  sockets.push(ws);
  const queue = [];
  const waiters = [];
  ws.on("message", (raw) => {
    const text = raw.toString();
    const m = text === "pong" ? { type: "pong" } : JSON.parse(text);
    const i = waiters.findIndex((w) => w.type === m.type);
    if (i >= 0) {
      const [w] = waiters.splice(i, 1);
      clearTimeout(w.timer);
      w.resolve(m);
    } else queue.push(m);
  });
  await new Promise((resolve, reject) => {
    ws.once("open", resolve);
    ws.once("error", reject);
  });
  return {
    ws,
    send: (m) => ws.send(JSON.stringify(m)),
    closed: () => new Promise((resolve) => (ws.readyState === WebSocket.CLOSED ? resolve() : ws.once("close", resolve))),
    next: (type) => {
      const i = queue.findIndex((m) => m.type === type);
      if (i >= 0) return Promise.resolve(queue.splice(i, 1)[0]);
      return new Promise((resolve, reject) => {
        const w = { type, resolve, timer: setTimeout(() => reject(Error(`Timed out: ${type}`)), 5000) };
        waiters.push(w);
      });
    },
  };
}
const hostAuth = (key = secret) => ({ type: "auth", version: 2, role: "host", secret: key });
async function pair(code) {
  const response = await fetch(`${http}/pair`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ code }),
  });
  return { status: response.status, ...(await response.json()) };
}
/** Full viewer join: POST the code, then authenticate the socket with the ticket. */
async function joinWithCode(code) {
  const claim = await pair(code);
  assert.equal(claim.status, 200, `pairing with a valid code: ${JSON.stringify(claim)}`);
  assert.equal(claim.room, room);
  const viewer = await connect(`/room/${claim.room}`);
  viewer.send({ type: "auth", version: 2, role: "viewer", ticket: claim.ticket });
  return { viewer, claim };
}
try {
  // A host must present the credential its room name is derived from.
  const impostor = await connect(`/room/${room}`);
  impostor.send(hostAuth("0".repeat(64)));
  assert.equal((await impostor.next("error")).code, "authentication");

  let host = await connect(`/room/${room}`);
  host.send(hostAuth());
  const hostAuthenticated = await host.next("authenticated");
  assert.equal(hostAuthenticated.role, "host");
  assert.deepEqual(hostAuthenticated.features, ["resume", "heartbeat"]);

  // No code registered yet: pairing is rejected before it reaches the room.
  assert.deepEqual(await pair(newCode()), { status: 404, error: "invalid-code" });
  assert.equal((await pair("bad")).error, "invalid-code");

  const codeA = newCode();
  host.send({ type: "codes", codes: [{ hash: sha256(codeA), ttlMs: 135000 }] });
  await new Promise((r) => setTimeout(r, 300));

  // Lowercase and separators are normalized; wrong codes fail.
  assert.equal((await pair(newCode())).error, "invalid-code");
  const { viewer, claim } = await joinWithCode(codeA.toLowerCase().slice(0, 3) + "-" + codeA.slice(3));
  const authenticated = await viewer.next("authenticated");
  assert.equal(authenticated.role, "viewer");
  assert.equal(authenticated.room, room);
  assert.match(authenticated.token, /^[a-f0-9]{64}$/);
  const h = await host.next("ready"), v = await viewer.next("ready");
  assert.equal(h.generation, v.generation);

  // Tickets are single use.
  const replay = await connect(`/room/${room}`);
  replay.send({ type: "auth", version: 2, role: "viewer", ticket: claim.ticket });
  assert.equal((await replay.next("error")).code, "authentication");

  // One viewer maximum.
  assert.deepEqual(await pair(codeA), { status: 409, error: "viewer-occupied" });

  // Rotation: publishing a new code with the old one still listed keeps both usable and does not disturb the session.
  const codeB = newCode();
  host.send({ type: "codes", codes: [{ hash: sha256(codeA), ttlMs: 15000 }, { hash: sha256(codeB), ttlMs: 135000 }] });
  await new Promise((r) => setTimeout(r, 300));
  const offer = { type: "offer", generation: h.generation, sdp: "v=0\r\n" };
  host.send(offer);
  assert.deepEqual(await viewer.next("offer"), offer);
  const answer = { ...offer, type: "answer" };
  viewer.send(answer);
  assert.deepEqual(await host.next("answer"), answer);
  const ice = { type: "ice", generation: h.generation, candidate: "candidate:1 1 UDP 1 127.0.0.1 9000 typ host", mid: "video" };
  host.send(ice);
  assert.deepEqual(await viewer.next("ice"), ice);

  // Heartbeat: the runtime answers "ping" with "pong".
  viewer.ws.send("ping");
  assert.equal((await viewer.next("pong")).type, "pong");

  // Resume: both peers report their media connection up; the host's WebSocket drops and comes back still holding
  // it. The worker hands back the same generation with resume: true, so nobody renegotiates.
  viewer.send({ type: "state", generation: h.generation, live: true });
  host.send({ type: "state", generation: h.generation, live: true });
  await new Promise((r) => setTimeout(r, 200));
  host.ws.close();
  await viewer.next("peer-left");
  host = await connect(`/room/${room}`);
  host.send({ ...hostAuth(), live: h.generation });
  await host.next("authenticated");
  const resumedHost = await host.next("ready"), resumedViewer = await viewer.next("ready");
  assert.equal(resumedHost.generation, h.generation);
  assert.equal(resumedHost.resume, true);
  assert.equal(resumedViewer.resume, true);
  host.send(ice); // Relays continue on the kept generation
  assert.deepEqual(await viewer.next("ice"), ice);

  // Viewer reload: the resume token works without a code, even after the code set changed.
  viewer.ws.close();
  await host.next("peer-left");
  const resumed = await connect(`/room/${room}`);
  resumed.send({ type: "auth", version: 2, role: "viewer", token: authenticated.token });
  await resumed.next("authenticated");
  const r = await host.next("ready");
  await resumed.next("ready");
  assert.notEqual(r.generation, h.generation);
  // An old generation must not reach the refreshed viewer.
  host.send(offer);
  host.send({ ...offer, generation: r.generation });
  assert.equal((await resumed.next("offer")).generation, r.generation);
  resumed.send({ ...offer, generation: r.generation });
  assert.equal((await resumed.next("error")).code, "role");
  await host.next("peer-left");

  // A bad resume token is rejected.
  const badToken = await connect(`/room/${room}`);
  badToken.send({ type: "auth", version: 2, role: "viewer", token: "f".repeat(64) });
  assert.equal((await badToken.next("error")).code, "authentication");

  // Disconnect Viewer: the host kicks; the viewer's token is revoked; the old code (still in overlap) works again.
  const { viewer: again } = await joinWithCode(codeA);
  const againAuth = await again.next("authenticated");
  await host.next("ready");
  await again.next("ready");
  host.send({ type: "kick" });
  await again.next("kicked");
  await again.closed();
  const revoked = await connect(`/room/${room}`);
  revoked.send({ type: "auth", version: 2, role: "viewer", token: againAuth.token });
  assert.equal((await revoked.next("error")).code, "authentication");

  // Viewers may not publish codes or kick.
  const { viewer: cheeky } = await joinWithCode(codeB);
  await cheeky.next("authenticated");
  await host.next("ready");
  cheeky.send({ type: "kick" });
  assert.equal((await cheeky.next("error")).code, "role");
  await host.next("peer-left");

  // A restarted host replaces the stale host socket instead of being locked out.
  const restarted = await connect(`/room/${room}`);
  restarted.send(hostAuth());
  await restarted.next("authenticated");
  await host.closed();

  // Without any host online a valid code reports that clearly.
  restarted.ws.close();
  await new Promise((r) => setTimeout(r, 300));
  assert.deepEqual(await pair(codeB), { status: 409, error: "host-unavailable" });

  // Oversized frames are rejected.
  const oversized = await connect(`/room/${room}`);
  oversized.ws.send("x".repeat(25000));
  assert.equal((await oversized.next("error")).code, "malformed");

  // Brute force: repeated wrong codes from one client are throttled.
  let limited = false;
  for (let i = 0; i < 14 && !limited; ++i) {
    const guess = await pair(newCode());
    if (guess.error === "rate-limit") limited = true;
    else assert.equal(guess.error, "invalid-code");
  }
  assert.ok(limited, "expected rate limiting after repeated wrong codes");
  console.log(
    "Signaling integration passed: host credential auth, code pairing, normalization, single-use tickets, one-viewer limit, rotation overlap, heartbeat, session resume, resume token, kick, roles, host replacement, host-unavailable, size limits, throttling.",
  );
} finally {
  for (const ws of sockets) ws.close();
}

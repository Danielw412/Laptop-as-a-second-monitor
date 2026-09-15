import WebSocket from "ws";
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";
const base = process.env.SIGNALING_URL ?? "ws://127.0.0.1:8787";
const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const room = Array.from(randomBytes(8), (b) => alphabet[b % 32]).join("");
const secret = randomBytes(32).toString("hex");
const sockets = [];
async function connect() {
  const ws = new WebSocket(`${base}/room/${room}`);
  sockets.push(ws);
  const queue = [];
  const waiters = [];
  ws.on("message", (raw) => {
    const m = JSON.parse(raw.toString());
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
    next: (type) => {
      const i = queue.findIndex((m) => m.type === type);
      if (i >= 0) return Promise.resolve(queue.splice(i, 1)[0]);
      return new Promise((resolve, reject) => {
        const w = {
          type,
          resolve,
          timer: setTimeout(() => reject(Error(`Timed out: ${type}`)), 5000),
        };
        waiters.push(w);
      });
    },
  };
}
const auth = (role, key = secret) => ({
  type: "auth",
  version: 1,
  role,
  secret: key,
});
try {
  const host = await connect();
  host.send(auth("host"));
  await host.next("authenticated");
  const bad = await connect();
  bad.send(auth("viewer", "0".repeat(64)));
  assert.equal((await bad.next("error")).code, "authentication");
  const viewer = await connect();
  viewer.send(auth("viewer"));
  await viewer.next("authenticated");
  const h = await host.next("ready"),
    v = await viewer.next("ready");
  assert.equal(h.generation, v.generation);
  const duplicate = await connect();
  duplicate.send(auth("viewer"));
  assert.equal((await duplicate.next("error")).code, "role-occupied");
  const offer = { type: "offer", generation: h.generation, sdp: "v=0\r\n" };
  host.send(offer);
  assert.deepEqual(await viewer.next("offer"), offer);
  const answer = { ...offer, type: "answer" };
  viewer.send(answer);
  assert.deepEqual(await host.next("answer"), answer);
  const ice = {
    type: "ice",
    generation: h.generation,
    candidate: "candidate:1 1 UDP 1 127.0.0.1 9000 typ host",
    mid: "video",
  };
  host.send(ice);
  assert.deepEqual(await viewer.next("ice"), ice);
  viewer.ws.close();
  await host.next("peer-left");
  const refreshed = await connect();
  refreshed.send(auth("viewer"));
  await refreshed.next("authenticated");
  const r = await host.next("ready");
  await refreshed.next("ready");
  assert.notEqual(r.generation, h.generation);
  // An old generation must not reach the refreshed viewer.
  host.send(offer);
  host.send({ ...offer, generation: r.generation });
  assert.equal((await refreshed.next("offer")).generation, r.generation);
  refreshed.send({ ...offer, generation: r.generation });
  assert.equal((await refreshed.next("error")).code, "role");
  await host.next("peer-left");
  const oversized = await connect();
  oversized.ws.send("x".repeat(25000));
  assert.equal((await oversized.next("error")).code, "malformed");
  console.log(
    "Signaling integration passed: authentication, roles, SDP, ICE, refresh, stale generation, size limits.",
  );
} finally {
  for (const ws of sockets) ws.close();
}

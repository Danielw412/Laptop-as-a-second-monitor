import "./style.css";
import { Session } from "./session";
import { Dashboard } from "./dashboard";
import { ROOM_RE, SECRET_RE } from "../../shared/protocol";
const el = <T extends HTMLElement>(id: string) =>
  document.getElementById(id) as T;
const server = el<HTMLInputElement>("server"),
  room = el<HTMLInputElement>("room"),
  secret = el<HTMLInputElement>("secret"),
  video = el<HTMLVideoElement>("video");
server.value = import.meta.env.VITE_SIGNALING_URL ?? "";
const dashboard = new Dashboard(el("dashboard-grid"));
const storageKey = "browser-monitor-pairing";
let resume = false;
try {
  const saved = JSON.parse(sessionStorage.getItem(storageKey) ?? "null");
  if (saved && ROOM_RE.test(saved.room) && SECRET_RE.test(saved.secret)) {
    server.value = saved.server; room.value = saved.room; secret.value = saved.secret; resume = true;
  }
} catch { /* Session storage can be disabled by browser policy. */ }
const params = new URLSearchParams(location.hash.slice(1));
if (params.has("room")) room.value = params.get("room")!;
if (params.has("secret")) secret.value = params.get("secret")!;
if (params.has("server")) server.value = params.get("server")!;
if (location.hash)
  history.replaceState(null, "", location.pathname + location.search);
let session: Session | undefined;
let patternTimer: ReturnType<typeof setInterval> | undefined;
let frameCallback: number | undefined;
function stop(forget = true) {
  if (forget) { try { sessionStorage.removeItem(storageKey); } catch {} }
  session?.stop();
  session = undefined;
  clearInterval(patternTimer);
  video.srcObject = null;
  if (frameCallback !== undefined) video.cancelVideoFrameCallback(frameCallback);
  dashboard.reset();
  el("pairing").hidden = false;
  el("screen").hidden = true;
  el<HTMLButtonElement>("disconnect").disabled = true;
  el<HTMLButtonElement>("fullscreen").disabled = true;
  el("status").textContent = "Disconnected";
}
function start(role: "host" | "viewer", stream?: MediaStream) {
  if (!ROOM_RE.test(room.value.toUpperCase()) || !SECRET_RE.test(secret.value)) throw Error("Enter a valid room code and 64-character session secret.");
  session?.stop();
  dashboard.reset();
  el("error").textContent = "";
  el<HTMLButtonElement>("disconnect").disabled = false;
  session = new Session(
    server.value,
    room.value.toUpperCase(),
    secret.value,
    role,
    (s) => (el("status").textContent = s),
    (s) => (el("error").textContent = s),
    (s) => {
      if (frameCallback !== undefined) video.cancelVideoFrameCallback(frameCallback);
      video.srcObject = s;
      const current = session;
      if ("requestVideoFrameCallback" in video)
        frameCallback = video.requestVideoFrameCallback(() => { if (session === current) current?.presented(); });
      el("pairing").hidden = true;
      el("screen").hidden = false;
      el<HTMLButtonElement>("fullscreen").disabled = false;
      void video.play().catch(() => (el("play").hidden = false));
    },
    (s) => dashboard.update(s),
    stream,
  );
  session.start();
  if (role === "viewer") {
    try { sessionStorage.setItem(storageKey, JSON.stringify({server:server.value, room:room.value.toUpperCase(), secret:secret.value})); } catch {}
  }
}
el("join").addEventListener("submit", (e) => {
  e.preventDefault();
  try {
    start("viewer");
  } catch (e) {
    el("error").textContent = String(e);
  }
});
room.addEventListener("input", () => (room.value = room.value.toUpperCase()));
el("disconnect").onclick = () => stop();
el("fullscreen").onclick = () =>
  void el("screen")
    .requestFullscreen()
    .catch((e) => (el("error").textContent = String(e)));
el("play").onclick = () =>
  void video.play().then(() => (el("play").hidden = true));
function pairing() {
  const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  room.value = Array.from(
    crypto.getRandomValues(new Uint8Array(8)),
    (v) => alphabet[v % 32],
  ).join("");
  secret.value = Array.from(crypto.getRandomValues(new Uint8Array(32)), (v) =>
    v.toString(16).padStart(2, "0"),
  ).join("");
  el("test-pairing").textContent =
    `Room: ${room.value}\nSecret: ${secret.value}`;
}
el("test-host").onclick = () =>
  void (async () => {
    try {
      const stream = await navigator.mediaDevices.getDisplayMedia({
        video: { width: 1920, height: 1080, frameRate: 60 },
        audio: false,
      });
      pairing();
      start("host", stream);
      stream.getVideoTracks()[0].onended = () => stop();
    } catch (e) {
      el("error").textContent = String(e);
    }
  })();
el("test-pattern").onclick = () => {
  try {
    const canvas = document.createElement("canvas");
    canvas.width = 1920;
    canvas.height = 1080;
    const ctx = canvas.getContext("2d")!;
    let frame = 0;
    clearInterval(patternTimer);
    patternTimer = setInterval(() => {
      ctx.fillStyle = "#15232b";
      ctx.fillRect(0, 0, 1920, 1080);
      ctx.fillStyle = "#adebc5";
      ctx.fillRect((frame * 12) % 1920, 0, 100, 1080);
      ctx.font = "80px monospace";
      ctx.fillText(`Browser Monitor  ${frame++}`, 200, 500);
    }, 1000 / 60);
    pairing();
    start("host", canvas.captureStream(60));
  } catch (e) {
    el("error").textContent = String(e);
  }
};
window.addEventListener("pagehide", () => stop(false));
if (resume || (params.has("room") && params.has("secret"))) {
  try { start("viewer"); } catch { el("error").textContent = "Saved pairing could not connect. Check the details and retry."; }
}

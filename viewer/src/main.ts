import "./style.css";
import { Session, type Credentials } from "./session";
import { Dashboard } from "./dashboard";
import { rememberServer, savedServer, usableSignalingUrl } from "./preferences";
import {
  CODE_ALPHABET,
  CODE_LENGTH,
  CODE_RE,
  ROOM_RE,
  TOKEN_RE,
  normalizeCode,
  roomForSecret,
} from "../../shared/protocol";
const el = <T extends HTMLElement>(id: string) =>
  document.getElementById(id) as T;
const server = el<HTMLInputElement>("server"),
  code = el<HTMLInputElement>("code"),
  video = el<HTMLVideoElement>("video");
const defaultServer = import.meta.env.VITE_SIGNALING_URL ?? "https://browser-monitor-signaling.danielruoqiao.workers.dev";
// A server entered once is kept for good (cookie, with localStorage as a fallback), so the only thing anyone has
// to type on a return visit is the pairing code.
server.value = savedServer() ?? defaultServer;
const dashboard = new Dashboard(el("dashboard-grid"));
const storageKey = "laptop-monitor-session";
type Saved = { server: string; room: string; token: string };
let saved: Saved | undefined;
try {
  const parsed = JSON.parse(sessionStorage.getItem(storageKey) ?? "null");
  if (parsed && typeof parsed.server === "string" && ROOM_RE.test(parsed.room) && TOKEN_RE.test(parsed.token))
    saved = parsed;
} catch { /* Session storage can be disabled by browser policy. */ }
const params = new URLSearchParams(location.hash.slice(1));
if (params.has("code")) code.value = normalizeCode(params.get("code")!);
if (params.has("server")) {
  // A link that carries a server wins over the remembered one, and replaces it from then on.
  server.value = params.get("server")!;
  rememberServer(server.value);
}
if (location.hash)
  history.replaceState(null, "", location.pathname + location.search);
let session: Session | undefined;
let patternTimer: ReturnType<typeof setInterval> | undefined;
let frameCallback: number | undefined;
function forget() {
  saved = undefined;
  try { sessionStorage.removeItem(storageKey); } catch {}
}
function stop(forgetSession = true) {
  if (forgetSession) forget();
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
function start(credentials: Credentials, stream?: MediaStream) {
  session?.stop();
  dashboard.reset();
  el("error").textContent = "";
  el<HTMLButtonElement>("disconnect").disabled = false;
  const current = new Session(
    server.value,
    credentials,
    (s) => (el("status").textContent = s),
    (s) => (el("error").textContent = s),
    (s) => {
      if (frameCallback !== undefined) video.cancelVideoFrameCallback(frameCallback);
      video.srcObject = s;
      if ("requestVideoFrameCallback" in video)
        frameCallback = video.requestVideoFrameCallback(() => { if (session === current) current.presented(); });
      el("pairing").hidden = true;
      el("screen").hidden = false;
      el<HTMLButtonElement>("fullscreen").disabled = false;
      void video.play().catch(() => (el("play").hidden = false));
    },
    (s) => dashboard.update(s),
    (p) => {
      if (credentials.role !== "viewer" || !p.room || !p.token) return;
      saved = { server: server.value, room: p.room, token: p.token };
      try { sessionStorage.setItem(storageKey, JSON.stringify(saved)); } catch {}
      rememberServer(server.value); // A server that actually paired is worth keeping.
    },
    (reason) => {
      // The host ended this session or rejected the credentials: never retry silently with the same ones.
      if (session === current) stop(reason !== "viewer-occupied");
      if (reason === "kicked") el("status").textContent = "Disconnected by the host. Enter the current code to reconnect.";
    },
    stream,
  );
  session = current;
  session.start();
}
el("join").addEventListener("submit", (e) => {
  e.preventDefault();
  try {
    const value = normalizeCode(code.value);
    code.value = value;
    if (!CODE_RE.test(value)) throw Error(`Enter the ${CODE_LENGTH}-character code shown in Laptop Monitor.`);
    rememberServer(server.value);
    forget();
    start({ role: "viewer", code: value });
  } catch (e) {
    el("error").textContent = e instanceof Error ? e.message : String(e);
  }
});
// Remembered as soon as it is edited, not only on a successful connect: a server typed into a page that is then
// closed without connecting is exactly the one nobody wants to type again.
server.addEventListener("change", () => rememberServer(server.value));
server.addEventListener("blur", () => rememberServer(server.value));
code.addEventListener("input", () => {
  const value = normalizeCode(code.value).slice(0, CODE_LENGTH);
  if (value !== code.value) code.value = value;
});
el("disconnect").onclick = () => stop();
el("fullscreen").onclick = () =>
  void el("screen")
    .requestFullscreen()
    .catch((e) => (el("error").textContent = String(e)));
el("play").onclick = () =>
  void video.play().then(() => (el("play").hidden = true));
function randomCode() {
  return Array.from(crypto.getRandomValues(new Uint8Array(CODE_LENGTH)), (v) => CODE_ALPHABET[v % 32]).join("");
}
async function testHost(stream: MediaStream) {
  const secret = Array.from(crypto.getRandomValues(new Uint8Array(32)), (v) => v.toString(16).padStart(2, "0")).join("");
  const pairingCode = randomCode();
  el("test-pairing").textContent = `Pairing code: ${pairingCode}`;
  start({ role: "host", room: await roomForSecret(secret), secret, code: pairingCode }, stream);
}
el("test-host").onclick = () =>
  void (async () => {
    try {
      const stream = await navigator.mediaDevices.getDisplayMedia({
        video: { width: 1920, height: 1080, frameRate: 60 },
        audio: false,
      });
      await testHost(stream);
      stream.getVideoTracks()[0].onended = () => stop();
    } catch (e) {
      el("error").textContent = String(e);
    }
  })();
el("test-pattern").onclick = () =>
  void (async () => {
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
        ctx.fillText(`Laptop Monitor  ${frame++}`, 200, 500);
      }, 1000 / 60);
      await testHost(canvas.captureStream(60));
    } catch (e) {
      el("error").textContent = String(e);
    }
  })();
window.addEventListener("pagehide", () => stop(false));
if (saved) {
  server.value = usableSignalingUrl(saved.server) ?? server.value;
  try { start({ role: "viewer", room: saved.room, token: saved.token }); } catch { el("error").textContent = "The previous session could not resume. Enter the current code."; }
} else if (CODE_RE.test(code.value)) {
  try { start({ role: "viewer", code: code.value }); } catch (e) { el("error").textContent = String(e); }
}

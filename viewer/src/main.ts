import "@fontsource-variable/instrument-sans";
import "@fontsource-variable/geist-mono";
import "./style.css";
import { Session, type Credentials } from "./session";
import { Dashboard } from "./dashboard";
import { Stage } from "./stage";
import {
  autoFullscreen,
  rememberAutoFullscreen,
  rememberServer,
  savedServer,
  usableSignalingUrl,
} from "./preferences";
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
const pairing = el("pairing"),
  join = el<HTMLFormElement>("join"),
  code = el<HTMLInputElement>("code"),
  server = el<HTMLInputElement>("server"),
  connect = el<HTMLButtonElement>("connect"),
  cancel = el<HTMLButtonElement>("cancel"),
  message = el("message"),
  fullscreenPreference = el<HTMLInputElement>("auto-fullscreen");
// The Pages build passes VITE_SIGNALING_URL through from a repository variable, which is an empty string when
// that variable is unset - so this falls back on anything falsy, not only on undefined. `??` would have left the
// field blank, which is the one thing a receiver page must never do.
const defaultServer =
  import.meta.env.VITE_SIGNALING_URL || "https://browser-monitor-signaling.danielruoqiao.workers.dev";
// A server entered once is kept for good (cookie, with localStorage as a fallback), so the only thing anyone has
// to type on a return visit is the pairing code.
server.value = savedServer() ?? defaultServer;
fullscreenPreference.checked = autoFullscreen();
fullscreenPreference.addEventListener("change", () => rememberAutoFullscreen(fullscreenPreference.checked));
const dashboard = new Dashboard(el("dashboard-grid"));
const stage = new Stage(el("stage"), {
  disconnect: () => stop(),
  mark: (rtp) => {
    session?.note(`picture marked as damaged (rtp ${rtp ?? "unknown"})`);
    session?.sendToHost({ type: "mark", rtp });
  },
});
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
let onStage = false;
let lastStatus = "";
let lastError = "";
let patternTimer: ReturnType<typeof setInterval> | undefined;
/** The one line under the code field: what is happening, or what went wrong. */
function say(text: string, error = false) {
  message.textContent = text;
  message.classList.toggle("error", error && !!text);
}
function showStage() {
  if (onStage) return;
  onStage = true;
  pairing.hidden = true;
  stage.show(autoFullscreen());
}
function showPairing() {
  if (!onStage) return;
  onStage = false;
  stage.hide();
  pairing.hidden = false;
  code.focus();
  code.select();
}
function setBusy(busy: boolean) {
  connect.disabled = busy;
  connect.textContent = busy ? "Connecting…" : "Connect display";
  cancel.hidden = !busy;
}
function forget() {
  saved = undefined;
  try { sessionStorage.removeItem(storageKey); } catch {}
}
function stop(forgetSession = true) {
  if (forgetSession) forget();
  session?.stop();
  session = undefined;
  clearInterval(patternTimer);
  dashboard.reset();
  showPairing();
  setBusy(false);
  say("Disconnected.");
}
function start(credentials: Credentials, stream?: MediaStream) {
  session?.stop();
  dashboard.reset();
  lastError = "";
  code.removeAttribute("aria-invalid");
  say("");
  setBusy(true);
  const current = new Session(
    server.value,
    credentials,
    {
      status: (s) => {
        if (session !== current) return;
        lastStatus = s;
        if (onStage) stage.status(s);
        else say(s);
      },
      error: (s) => {
        if (session !== current) return;
        lastError = s;
        if (onStage) stage.error(s);
        else if (s) say(s, true);
        // The pairing page has one line for both: clearing an error brings the last status back, not a blank.
        else if (message.classList.contains("error")) say(lastStatus);
      },
      video: (s) => {
        if (session === current) stage.attach(s, () => current.presented());
      },
      diagnostics: (d) => dashboard.update(d),
      probe: (rtp, cols, rows) => stage.probe(rtp, cols, rows),
      // The page moves to the stage as soon as the code has been accepted, so the fullscreen request still falls
      // inside the gesture that submitted the form. Only viewers have a picture to show; the sender test stays put.
      phase: (p) => {
        if (session !== current || credentials.role !== "viewer" || p === "pairing") return;
        showStage();
        stage.setLive(p === "connected");
      },
      paired: (p) => {
        if (credentials.role !== "viewer" || !p.room || !p.token) return;
        saved = { server: server.value, room: p.room, token: p.token };
        try { sessionStorage.setItem(storageKey, JSON.stringify(saved)); } catch {}
        rememberServer(server.value); // A server that actually paired is worth keeping.
      },
      ended: (reason) => {
        // The host ended this session or rejected the credentials: never retry silently with the same ones.
        if (session !== current) return;
        stop(reason !== "viewer-occupied");
        if (reason === "invalid-code") code.setAttribute("aria-invalid", "true");
        if (reason === "kicked") say("Disconnected by the host. Enter the current code to reconnect.");
        else if (lastError) say(lastError, true);
      },
    },
    stream,
  );
  session = current;
  try {
    current.start();
  } catch (e) {
    session = undefined;
    setBusy(false);
    throw e;
  }
}
const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));
join.addEventListener("submit", (e) => {
  e.preventDefault();
  const value = normalizeCode(code.value);
  code.value = value;
  if (!CODE_RE.test(value)) {
    code.setAttribute("aria-invalid", "true");
    code.focus();
    say(`Enter the ${CODE_LENGTH}-character code shown in Laptop Monitor.`, true);
    return;
  }
  rememberServer(server.value);
  forget();
  try {
    start({ role: "viewer", code: value });
  } catch (e) {
    say(errorText(e), true);
  }
});
cancel.onclick = () => stop();
// Remembered as soon as it is edited, not only on a successful connect: a server typed into a page that is then
// closed without connecting is exactly the one nobody wants to type again.
server.addEventListener("change", () => rememberServer(server.value));
server.addEventListener("blur", () => rememberServer(server.value));
code.addEventListener("input", () => {
  const value = normalizeCode(code.value).slice(0, CODE_LENGTH);
  if (value !== code.value) code.value = value;
  code.removeAttribute("aria-invalid");
  // Six characters is the whole code: connect without a second step.
  if (value.length === CODE_LENGTH && !session) join.requestSubmit();
});
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
      say(errorText(e), true);
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
        ctx.fillStyle = "#2b2119";
        ctx.fillRect(0, 0, 1920, 1080);
        ctx.fillStyle = "#ffdbbb";
        ctx.fillRect((frame * 12) % 1920, 0, 100, 1080);
        ctx.font = "80px monospace";
        ctx.fillText(`Laptop Monitor  ${frame++}`, 200, 500);
      }, 1000 / 60);
      await testHost(canvas.captureStream(60));
    } catch (e) {
      say(errorText(e), true);
    }
  })();
window.addEventListener("pagehide", () => stop(false));
// Things outside the connection that explain a stutter or a drop, recorded in the host's log with the rest.
document.addEventListener("visibilitychange", () => session?.note(`page ${document.visibilityState}`));
window.addEventListener("online", () => session?.note("network online"));
window.addEventListener("offline", () => session?.note("network offline"));
if (saved) {
  server.value = usableSignalingUrl(saved.server) ?? server.value;
  try { start({ role: "viewer", room: saved.room, token: saved.token }); } catch { say("The previous session could not resume. Enter the current code.", true); }
} else if (CODE_RE.test(code.value)) {
  try { start({ role: "viewer", code: code.value }); } catch (e) { say(errorText(e), true); }
}

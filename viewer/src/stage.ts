/**
 * The stage is the virtual display itself: the video, a status overlay while there is no picture, a small toolbar
 * that appears on mouse movement, and the connection panel. Once video is live the cursor and the toolbar hide
 * after a moment of stillness, so what is on screen is the other computer's desktop and nothing else.
 *
 * Fullscreen needs a user gesture in every browser, so the stage tries once when it is shown and otherwise waits
 * for a click on the picture, the F key or the toolbar. A screen wake lock keeps the receiving laptop from dimming
 * while it is being used as a monitor.
 */
const IDLE_MS = 2500;
export interface StageActions {
  disconnect(): void;
}
export class Stage {
  private readonly video: HTMLVideoElement;
  private readonly statusEl: HTMLElement;
  private readonly errorEl: HTMLElement;
  private readonly playButton: HTMLButtonElement;
  private readonly note: HTMLElement;
  private readonly fullscreenButton: HTMLButtonElement;
  private readonly statsButton: HTMLButtonElement;
  private readonly dashboard: HTMLElement;
  private idleTimer?: ReturnType<typeof setTimeout>;
  private frameCallback?: number;
  private wakeLock?: WakeLockSentinel;
  private shown = false;
  private live = false;
  private wantFullscreen = false;
  private everFullscreen = false;
  constructor(
    private readonly root: HTMLElement,
    actions: StageActions,
  ) {
    const el = <T extends HTMLElement>(id: string) => root.querySelector<T>(`#${id}`)!;
    this.video = el<HTMLVideoElement>("video");
    this.statusEl = el("stage-status");
    this.errorEl = el("stage-error");
    this.playButton = el<HTMLButtonElement>("play");
    this.note = el("toolbar-note");
    this.fullscreenButton = el<HTMLButtonElement>("fullscreen");
    this.statsButton = el<HTMLButtonElement>("stats");
    this.dashboard = el("dashboard");
    this.fullscreenButton.onclick = () => void this.toggleFullscreen();
    this.statsButton.onclick = () => this.toggleStats();
    el<HTMLButtonElement>("disconnect").onclick = () => actions.disconnect();
    this.playButton.onclick = () =>
      void this.video
        .play()
        .then(() => (this.playButton.hidden = true))
        .catch(() => {});
    root.addEventListener("pointermove", () => this.wake());
    root.addEventListener("pointerdown", () => this.wake());
    root.addEventListener("pointerleave", () => this.rest());
    // A click on the picture is the one gesture a viewer is sure to make, so it is enough to fill the screen.
    root.addEventListener("click", (e) => {
      if ((e.target as Element).closest("button, .toolbar, .dashboard")) return;
      if (!document.fullscreenElement) void this.enterFullscreen();
    });
    document.addEventListener("fullscreenchange", () => this.fullscreenChanged());
    document.addEventListener("keydown", (e) => this.key(e));
    document.addEventListener("visibilitychange", () => {
      if (document.visibilityState === "visible" && this.shown) void this.keepAwake();
    });
  }
  /** Shows the stage; with `fullscreen` it also asks for fullscreen once, which works only inside a user gesture. */
  show(fullscreen: boolean) {
    if (this.shown) return;
    this.shown = true;
    this.wantFullscreen = fullscreen;
    this.everFullscreen = false;
    this.root.hidden = false;
    this.setLive(false);
    void this.keepAwake();
    if (fullscreen) {
      void this.root.offsetHeight; // Laid out before the request, so the browser has something to make fullscreen.
      void this.enterFullscreen();
    }
  }
  hide() {
    if (!this.shown) return;
    this.shown = false;
    if (document.fullscreenElement === this.root) void document.exitFullscreen().catch(() => {});
    this.setLive(false);
    this.detach();
    this.status("");
    this.error("");
    this.dashboard.hidden = true;
    this.statsButton.setAttribute("aria-pressed", "false");
    this.root.hidden = true;
    void this.wakeLock?.release().catch(() => {});
    this.wakeLock = undefined;
  }
  attach(stream: MediaStream, presented: () => void) {
    this.detach();
    this.video.srcObject = stream;
    if ("requestVideoFrameCallback" in this.video)
      this.frameCallback = this.video.requestVideoFrameCallback(() => presented());
    void this.video.play().catch(() => (this.playButton.hidden = false));
  }
  detach() {
    if (this.frameCallback !== undefined) {
      this.video.cancelVideoFrameCallback(this.frameCallback);
      this.frameCallback = undefined;
    }
    this.video.srcObject = null;
    this.playButton.hidden = true;
  }
  /** Live means a picture is on screen: the overlay goes, and the cursor and toolbar hide when still. */
  setLive(on: boolean) {
    this.live = on;
    this.root.classList.toggle("live", on);
    if (on) this.wake();
    else this.rest();
    this.updateNote();
  }
  status(text: string) {
    this.statusEl.textContent = text;
  }
  error(text: string) {
    this.errorEl.textContent = text;
    this.errorEl.hidden = !text;
  }
  private async enterFullscreen(): Promise<boolean> {
    if (document.fullscreenElement === this.root) return true;
    try {
      await this.root.requestFullscreen({ navigationUI: "hide" });
      return true;
    } catch {
      return false; // No gesture behind the request, or the browser does not allow it: the click hint stays.
    }
  }
  private async toggleFullscreen() {
    if (document.fullscreenElement === this.root) await document.exitFullscreen().catch(() => {});
    else await this.enterFullscreen();
  }
  private toggleStats() {
    this.dashboard.hidden = !this.dashboard.hidden;
    this.statsButton.setAttribute("aria-pressed", String(!this.dashboard.hidden));
    this.wake();
  }
  private fullscreenChanged() {
    const on = document.fullscreenElement === this.root;
    if (on) this.everFullscreen = true;
    this.fullscreenButton.textContent = on ? "Exit fullscreen" : "Fullscreen";
    this.updateNote();
    this.wake();
  }
  private updateNote() {
    const show =
      this.live && this.wantFullscreen && !this.everFullscreen && document.fullscreenElement !== this.root;
    this.note.textContent = show ? "Click the picture to fill the screen" : "";
    this.note.hidden = !show;
  }
  private key(e: KeyboardEvent) {
    if (!this.shown || e.ctrlKey || e.metaKey || e.altKey) return;
    const target = e.target as HTMLElement | null;
    if (target && ["INPUT", "TEXTAREA", "SELECT"].includes(target.tagName)) return;
    if (e.key === "f" || e.key === "F") {
      e.preventDefault();
      void this.toggleFullscreen();
    } else if (e.key === "s" || e.key === "S") {
      e.preventDefault();
      this.toggleStats();
    }
  }
  /** Cursor and toolbar come back on any movement, and go again after a moment if a picture is showing. */
  private wake() {
    this.root.classList.remove("idle");
    clearTimeout(this.idleTimer);
    if (this.live) this.idleTimer = setTimeout(() => this.root.classList.add("idle"), IDLE_MS);
  }
  private rest() {
    clearTimeout(this.idleTimer);
    this.root.classList.toggle("idle", this.live);
  }
  private async keepAwake() {
    if (!("wakeLock" in navigator) || this.wakeLock) return;
    try {
      const lock = await navigator.wakeLock.request("screen");
      lock.addEventListener("release", () => {
        if (this.wakeLock === lock) this.wakeLock = undefined;
      });
      this.wakeLock = lock;
    } catch {
      /* Refused (battery saver, hidden tab, policy): the display still works, the laptop may just dim. */
    }
  }
}

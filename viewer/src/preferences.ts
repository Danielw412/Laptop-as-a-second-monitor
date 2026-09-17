/**
 * The one setting this page has: which signaling server to use. It is remembered in a cookie so it survives
 * closing the tab, the browser and the machine - a receiver is usually paired once and then used for months, and
 * retyping a workers.dev URL on a phone keyboard is its own small punishment.
 *
 * A cookie can be refused (third-party blocking, private modes, file://), so localStorage is kept as a fallback
 * and both are written. Neither is required for the page to work; a blocked store just means the compiled-in
 * default comes back.
 */
const KEY = "lm_signaling_url";
const MAX_AGE_SECONDS = 60 * 60 * 24 * 365;

/** Parses a document.cookie header. Exported for tests: it never touches the DOM itself. */
export function parseCookies(header: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const part of header.split(";")) {
    const eq = part.indexOf("=");
    if (eq < 1) continue;
    const name = part.slice(0, eq).trim();
    if (!name) continue;
    try {
      out[name] = decodeURIComponent(part.slice(eq + 1).trim());
    } catch {
      // A value that is not valid percent-encoding is not one we wrote; ignore it.
    }
  }
  return out;
}

/**
 * The same rule the session enforces before connecting: HTTPS everywhere, HTTP only on localhost. A stored value
 * that would be rejected later is treated as absent now, so a stale or tampered cookie can never leave the page
 * stuck on a server it refuses to use.
 */
export function usableSignalingUrl(value: string | undefined | null): string | undefined {
  if (!value || value.length > 512) return undefined;
  let url: URL;
  try {
    url = new URL(value);
  } catch {
    return undefined;
  }
  const local = ["localhost", "127.0.0.1"].includes(url.hostname);
  if (url.protocol !== "https:" && !(url.protocol === "http:" && local)) return undefined;
  url.search = "";
  url.hash = "";
  return url.toString();
}

/** Cookies are scoped to this app's own directory: one github.io origin can host many unrelated pages. */
function cookiePath(): string {
  const path = location.pathname;
  return path.slice(0, path.lastIndexOf("/") + 1) || "/";
}

export function savedServer(): string | undefined {
  let stored: string | undefined;
  try {
    stored = parseCookies(document.cookie)[KEY];
  } catch {
    /* Cookies unavailable. */
  }
  if (!stored)
    try {
      stored = localStorage.getItem(KEY) ?? undefined;
    } catch {
      /* Storage disabled by browser policy. */
    }
  return usableSignalingUrl(stored);
}

export function rememberServer(value: string): void {
  const url = usableSignalingUrl(value);
  if (!url) return; // Half-typed or unusable: keep whatever was remembered last.
  const secure = location.protocol === "https:" ? "; Secure" : "";
  try {
    document.cookie = `${KEY}=${encodeURIComponent(url)}; Max-Age=${MAX_AGE_SECONDS}; Path=${cookiePath()}; SameSite=Lax${secure}`;
  } catch {
    /* Cookies unavailable; the fallback below still applies. */
  }
  try {
    localStorage.setItem(KEY, url);
  } catch {
    /* Storage disabled by browser policy. */
  }
}

export function forgetServer(): void {
  try {
    document.cookie = `${KEY}=; Max-Age=0; Path=${cookiePath()}; SameSite=Lax`;
  } catch {
    /* Cookies unavailable. */
  }
  try {
    localStorage.removeItem(KEY);
  } catch {
    /* Storage disabled by browser policy. */
  }
}

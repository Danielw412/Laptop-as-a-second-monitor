import { it, expect } from "vitest";
import { parseAutoFullscreen, parseCookies, usableSignalingUrl } from "../viewer/src/preferences";

it("reads a percent-encoded value back out of a cookie header", () => {
  const header = "other=1; lm_signaling_url=https%3A%2F%2Fexample.workers.dev; last=2";
  expect(parseCookies(header).lm_signaling_url).toBe("https://example.workers.dev");
});
it("ignores malformed cookie parts rather than throwing", () => {
  expect(parseCookies("; =novalue; broken; good=1")).toEqual({ good: "1" });
  expect(parseCookies("bad=%E0%A4%A").good).toBeUndefined();
});
it("keeps HTTPS servers and localhost over HTTP", () => {
  expect(usableSignalingUrl("https://example.workers.dev")).toBe("https://example.workers.dev/");
  expect(usableSignalingUrl("http://127.0.0.1:8788")).toBe("http://127.0.0.1:8788/");
  expect(usableSignalingUrl("http://localhost:8788/")).toBe("http://localhost:8788/");
});
it("refuses a stored value the session would reject anyway", () => {
  expect(usableSignalingUrl("http://example.com")).toBeUndefined();
  expect(usableSignalingUrl("javascript:alert(1)")).toBeUndefined();
  expect(usableSignalingUrl("not a url")).toBeUndefined();
  expect(usableSignalingUrl("")).toBeUndefined();
  expect(usableSignalingUrl(undefined)).toBeUndefined();
  expect(usableSignalingUrl("https://example.com/" + "a".repeat(600))).toBeUndefined();
});
it("drops a query and fragment so the stored address is just the server", () => {
  expect(usableSignalingUrl("https://example.workers.dev/room?x=1#y")).toBe("https://example.workers.dev/room");
});
it("fills the screen unless that was explicitly turned off", () => {
  expect(parseAutoFullscreen(null)).toBe(true);
  expect(parseAutoFullscreen(undefined)).toBe(true);
  expect(parseAutoFullscreen("on")).toBe(true);
  expect(parseAutoFullscreen("garbage")).toBe(true);
  expect(parseAutoFullscreen("off")).toBe(false);
});

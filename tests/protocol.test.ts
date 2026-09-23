import { describe, it, expect } from "vitest";
import {
  CODE_RE,
  ROOM_RE,
  normalizeCode,
  parseClient,
  roomForSecret,
  sha256Hex,
} from "../shared/protocol";
const generation = "12345678-1234-1234-1234-123456789012";
describe("signaling protocol v2", () => {
  it("authenticates a host with its 256-bit credential", () =>
    expect(
      parseClient(JSON.stringify({ type: "auth", version: 2, role: "host", secret: "a".repeat(64) })).type,
    ).toBe("auth"));
  it("authenticates a viewer by one-time ticket or by resume token, never by secret", () => {
    expect(
      parseClient(JSON.stringify({ type: "auth", version: 2, role: "viewer", ticket: "c".repeat(64) })),
    ).toEqual({ type: "auth", version: 2, role: "viewer", ticket: "c".repeat(64) });
    expect(
      parseClient(JSON.stringify({ type: "auth", version: 2, role: "viewer", token: "b".repeat(64) })),
    ).toEqual({ type: "auth", version: 2, role: "viewer", token: "b".repeat(64) });
    expect(() => parseClient(JSON.stringify({ type: "auth", version: 2, role: "viewer" }))).toThrow();
    expect(() =>
      parseClient(
        JSON.stringify({ type: "auth", version: 2, role: "viewer", ticket: "c".repeat(64), token: "b".repeat(64) }),
      ),
    ).toThrow();
  });
  it.each([
    "{}",
    "null",
    "[]",
    '{"type":"video"}',
    JSON.stringify({ type: "auth", version: 1, role: "host", secret: "a".repeat(64) }),
    JSON.stringify({ type: "auth", version: 2, role: "host", secret: "ABCDEF" }),
    JSON.stringify({ type: "auth", version: 2, role: "viewer", token: "short" }),
    JSON.stringify({ type: "auth", version: 2, role: "viewer", secret: "a".repeat(64) }),
    JSON.stringify({ type: "codes", codes: [{ hash: "zz", ttlMs: 1000 }] }),
    JSON.stringify({ type: "codes", codes: [{ hash: "a".repeat(64), ttlMs: 0 }] }),
    JSON.stringify({ type: "codes", codes: [{ hash: "a".repeat(64), ttlMs: 999_999_999 }] }),
    JSON.stringify({ type: "codes", codes: [1, 2, 3] }),
    "a".repeat(25000),
  ])("rejects invalid input %s", (raw) => expect(() => parseClient(raw)).toThrow());
  it("accepts at most two code registrations", () => {
    const code = { hash: "c".repeat(64), ttlMs: 135000 };
    expect(parseClient(JSON.stringify({ type: "codes", codes: [code, code] }))).toEqual({
      type: "codes",
      codes: [code, code],
    });
    expect(() => parseClient(JSON.stringify({ type: "codes", codes: [code, code, code] }))).toThrow();
  });
  it("parses kick", () => expect(parseClient('{"type":"kick"}')).toEqual({ type: "kick" }));
  it("validates and strips unknown fields", () =>
    expect(
      parseClient(JSON.stringify({ type: "offer", generation, sdp: "v=0\r\n", secret: "do not forward" })),
    ).toEqual({ type: "offer", generation, sdp: "v=0\r\n" }));
  it("rejects arbitrary data in ICE", () =>
    expect(() =>
      parseClient(JSON.stringify({ type: "ice", generation, candidate: "video", mid: "0" })),
    ).toThrow());
  it("carries the generation a reconnecting peer still has media for, and drops anything else there", () => {
    expect(
      parseClient(JSON.stringify({ type: "auth", version: 2, role: "host", secret: "a".repeat(64), live: generation })),
    ).toEqual({ type: "auth", version: 2, role: "host", secret: "a".repeat(64), live: generation });
    expect(
      parseClient(JSON.stringify({ type: "auth", version: 2, role: "viewer", token: "b".repeat(64), live: generation })),
    ).toEqual({ type: "auth", version: 2, role: "viewer", token: "b".repeat(64), live: generation });
    expect(
      parseClient(JSON.stringify({ type: "auth", version: 2, role: "viewer", token: "b".repeat(64), live: "<script>" })),
    ).toEqual({ type: "auth", version: 2, role: "viewer", token: "b".repeat(64) });
  });
  it("parses media state reports and nothing more", () => {
    expect(parseClient(JSON.stringify({ type: "state", generation, live: true, extra: 1 }))).toEqual({
      type: "state",
      generation,
      live: true,
    });
    expect(() => parseClient(JSON.stringify({ type: "state", generation, live: "yes" }))).toThrow();
    expect(() => parseClient(JSON.stringify({ type: "state", generation: "x", live: true }))).toThrow();
  });
});
describe("pairing codes", () => {
  it("uses the 32-symbol alphabet without I, O, 0 or 1", () => {
    expect(CODE_RE.test("K7M4Q2")).toBe(true);
    expect(CODE_RE.test("K7M4Q")).toBe(false);
    expect(CODE_RE.test("K7M4Q21")).toBe(false);
    expect(CODE_RE.test("I0O1AB")).toBe(false);
    expect(CODE_RE.test("k7m4q2")).toBe(false);
  });
  it("normalizes user input", () => {
    expect(normalizeCode("k7m 4q2")).toBe("K7M4Q2");
    expect(normalizeCode("K7M-4Q2")).toBe("K7M4Q2");
    expect(normalizeCode(" k7m4q2\n")).toBe("K7M4Q2");
  });
  it("derives the room id from the credential deterministically", async () => {
    const secret = "0123456789abcdef".repeat(4);
    const room = await roomForSecret(secret);
    expect(ROOM_RE.test(room)).toBe(true);
    expect(room).toBe((await sha256Hex(secret)).slice(0, 32));
    expect(await roomForSecret("f".repeat(64))).not.toBe(room);
  });
  it("matches the host implementation's test vector", async () => {
    // Shared with host/tests/logic_tests.cpp: SHA-256("abc") is the canonical vector.
    expect(await sha256Hex("abc")).toBe("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect(await roomForSecret("abc")).toBe("ba7816bf8f01cfea414140de5dae2223");
  });
});

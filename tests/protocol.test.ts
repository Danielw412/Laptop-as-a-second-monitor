import { describe, it, expect } from "vitest";
import { parseClient, ROOM_RE } from "../shared/protocol";
const generation = "12345678-1234-1234-1234-123456789012";
describe("signaling protocol", () => {
  it("authenticates with a separate 256-bit secret", () =>
    expect(
      parseClient(
        JSON.stringify({
          type: "auth",
          version: 1,
          role: "host",
          secret: "a".repeat(64),
        }),
      ).type,
    ).toBe("auth"));
  it.each([
    "{}",
    "null",
    "[]",
    '{"type":"video"}',
    JSON.stringify({
      type: "auth",
      version: 1,
      role: "viewer",
      secret: "ABCDEFGH",
    }),
    "a".repeat(25000),
  ])("rejects invalid input %s", (raw) =>
    expect(() => parseClient(raw)).toThrow(),
  );
  it("validates and strips unknown fields", () =>
    expect(
      parseClient(
        JSON.stringify({
          type: "offer",
          generation,
          sdp: "v=0\r\n",
          secret: "do not forward",
        }),
      ),
    ).toEqual({ type: "offer", generation, sdp: "v=0\r\n" }));
  it("rejects invalid room alphabet", () => {
    expect(ROOM_RE.test("ABCDEFGH")).toBe(true);
    expect(ROOM_RE.test("IIII0000")).toBe(false);
  });
  it("rejects arbitrary data in ICE", () =>
    expect(() =>
      parseClient(
        JSON.stringify({
          type: "ice",
          generation,
          candidate: "video",
          mid: "0",
        }),
      ),
    ).toThrow());
});

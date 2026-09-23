import { describe, expect, it } from "vitest";
import { base64, lumaFromRgb, lumaGrid, lumaGridFromPlane, rtpAfter } from "../viewer/src/probe";
describe("receiver probe grid", () => {
  it("maps RGB to limited-range BT.709 luma like the host", () => {
    expect(lumaFromRgb(0, 0, 0)).toBe(16);
    expect(lumaFromRgb(255, 255, 255)).toBe(235);
    expect(lumaFromRgb(255, 0, 0)).toBe(63); // 16 + 0.2126 * 219 = 62.6, rounded
  });
  it("computes per-cell mean and detail with the host's cell boundaries and rounding", () => {
    // 4x2 image, 2x1 grid: left cell alternates black/white, right cell is flat grey.
    const w = 4, h = 2;
    const rgba = new Uint8ClampedArray(w * h * 4);
    for (let y = 0; y < h; ++y)
      for (let x = 0; x < w; ++x) {
        const v = x < 2 ? (x % 2 ? 255 : 0) : 128;
        rgba.set([v, v, v, 255], (y * w + x) * 4);
      }
    const g = lumaGrid(rgba, w, h, 2, 1);
    expect(g.cols).toBe(2);
    expect(Array.from(g.mean)).toEqual([126, 126]); // (16 + 235) / 2 rounded; grey 128 -> 126
    // Left: |235 - 16| = 219 per pair, times 4, capped at 255. Right: flat.
    expect(Array.from(g.detail)).toEqual([255, 0]);
    expect(base64(g.mean)).toBe("fn4=");
  });
  it("reads a decoded luma plane through its stride, ignoring the padding", () => {
    // 4x2 visible picture in rows of 6 bytes; the padding bytes (255) must not count.
    const plane = new Uint8Array([16, 16, 100, 100, 255, 255, 16, 16, 100, 100, 255, 255]);
    const g = lumaGridFromPlane(plane, 4, 2, 6, 2, 1);
    expect(Array.from(g.mean)).toEqual([16, 100]);
    expect(Array.from(g.detail)).toEqual([0, 0]);
  });
  it("orders RTP timestamps across the 32-bit wrap", () => {
    expect(rtpAfter(10, 5)).toBe(true);
    expect(rtpAfter(5, 10)).toBe(false);
    expect(rtpAfter(3, 0xfffffff0)).toBe(true);
    expect(rtpAfter(7, 7)).toBe(false);
  });
});

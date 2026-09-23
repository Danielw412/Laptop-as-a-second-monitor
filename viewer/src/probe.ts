/**
 * The receiver's half of a quality probe: the luma grid of one decoded frame, computed exactly the way the host
 * computes it for the NV12 surface it encoded (host/include/probe.hpp), so the host can compare the two. Per cell:
 * the mean luma (limited range, as the decoder outputs it) and the mean absolute difference between horizontal
 * neighbours (detail, which collapses when a region turns into flat blocks).
 *
 * Frames are read from the track itself (MediaStreamTrackProcessor, Chrome and Edge), which works while the page is
 * hidden and hands over the decoder's own NV12 output with the frame's RTP timestamp. Elsewhere probes report
 * themselves missed.
 */
/** The host's default grid (host/include/probe.hpp: 8x8-pixel cells at 1080p); a request may ask for another. */
export const GRID_COLUMNS = 240;
export const GRID_ROWS = 135;
export type Grid = { cols: number; rows: number; mean: Uint8Array; detail: Uint8Array };
/** Limited-range luma of an sRGB pixel, as lumaFromRgb in probe.hpp. */
export function lumaFromRgb(r: number, g: number, b: number): number {
  const y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  return Math.floor(Math.min(255, Math.max(0, 16 + (y * 219) / 255 + 0.5)));
}
/** Grid of an 8-bit luma plane with the given stride, as lumaCells in probe.hpp. */
export function lumaGridFromPlane(
  luma: Uint8Array, width: number, height: number, stride: number, cols = GRID_COLUMNS, rows = GRID_ROWS,
): Grid {
  const mean = new Uint8Array(cols * rows), detail = new Uint8Array(cols * rows);
  for (let r = 0; r < rows; ++r) {
    const y0 = Math.floor((r * height) / rows), y1 = Math.floor(((r + 1) * height) / rows);
    for (let c = 0; c < cols; ++c) {
      const x0 = Math.floor((c * width) / cols), x1 = Math.floor(((c + 1) * width) / cols);
      let sum = 0, gradient = 0, count = 0, pairs = 0;
      for (let y = y0; y < y1; ++y) {
        const row = y * stride;
        for (let x = x0; x < x1; ++x) {
          sum += luma[row + x];
          ++count;
          if (x + 1 < x1) {
            gradient += Math.abs(luma[row + x + 1] - luma[row + x]);
            ++pairs;
          }
        }
      }
      mean[r * cols + c] = count ? Math.floor((sum + Math.floor(count / 2)) / count) : 0;
      detail[r * cols + c] = Math.min(255, pairs ? Math.floor((gradient * 4 + Math.floor(pairs / 2)) / pairs) : 0);
    }
  }
  return { cols, rows, mean, detail };
}
/** Grid of an RGBA image (as getImageData returns it), for frames that are not planar YUV. */
export function lumaGrid(rgba: Uint8ClampedArray, width: number, height: number, cols = GRID_COLUMNS, rows = GRID_ROWS): Grid {
  const luma = new Uint8Array(width * height);
  for (let i = 0, p = 0; i < luma.length; ++i, p += 4) luma[i] = lumaFromRgb(rgba[p], rgba[p + 1], rgba[p + 2]);
  return lumaGridFromPlane(luma, width, height, width, cols, rows);
}
export function base64(bytes: Uint8Array): string {
  let text = "";
  for (const b of bytes) text += String.fromCharCode(b);
  return btoa(text);
}
/** True when RTP timestamp `a` is later than `b`, allowing for the 32-bit wrap. */
export function rtpAfter(a: number, b: number): boolean {
  const d = (a - b) >>> 0;
  return d !== 0 && d < 0x80000000;
}
// Chrome's track reader; not in TypeScript's DOM library because other engines lack it.
type FrameReader = ReadableStreamDefaultReader<VideoFrame>;
type ProcessorConstructor = new (init: { track: MediaStreamTrack }) => { readable: ReadableStream<VideoFrame> };
const Processor = (globalThis as { MediaStreamTrackProcessor?: ProcessorConstructor }).MediaStreamTrackProcessor;
export const canReadFrames = typeof Processor === "function";
export function frameRtp(frame: VideoFrame): number | undefined {
  const metadata = (frame as VideoFrame & { metadata?: () => { rtpTimestamp?: number } }).metadata?.();
  return typeof metadata?.rtpTimestamp === "number" ? metadata.rtpTimestamp : undefined;
}
/**
 * Reads decoded frames from the track until one satisfies `accept` (returned; the caller closes it), a later frame
 * shows the wanted one was skipped (`skipped` returns true), or the time runs out.
 */
export async function readFrame(
  track: MediaStreamTrack, accept: (frame: VideoFrame) => boolean, skipped: (frame: VideoFrame) => boolean,
  timeoutMs = 2000,
): Promise<VideoFrame | undefined> {
  if (!Processor) return undefined;
  const reader: FrameReader = new Processor({ track }).readable.getReader();
  const deadline = performance.now() + timeoutMs;
  try {
    while (performance.now() < deadline) {
      const next = await Promise.race([
        reader.read(),
        new Promise<null>((r) => setTimeout(() => r(null), Math.max(1, deadline - performance.now()))),
      ]);
      if (!next || next.done || !next.value) return undefined;
      const frame = next.value;
      if (accept(frame)) return frame;
      const giveUp = skipped(frame);
      frame.close();
      if (giveUp) return undefined;
    }
    return undefined;
  } finally {
    void reader.cancel().catch(() => {});
  }
}
/** The grid of a decoded frame: its own luma plane when it is planar YUV, otherwise drawn and converted. */
export async function frameGrid(frame: VideoFrame, cols = GRID_COLUMNS, rows = GRID_ROWS): Promise<Grid | undefined> {
  const rect = frame.visibleRect;
  if (!rect || cols > rect.width || rows > rect.height) return undefined;
  if (frame.format === "NV12" || frame.format === "I420" || frame.format === "I420A") {
    const buffer = new Uint8Array(frame.allocationSize({ rect }));
    const layout = await frame.copyTo(buffer, { rect });
    return lumaGridFromPlane(buffer.subarray(layout[0].offset), rect.width, rect.height, layout[0].stride, cols, rows);
  }
  if (typeof OffscreenCanvas === "undefined") return undefined;
  const canvas = new OffscreenCanvas(rect.width, rect.height);
  const context = canvas.getContext("2d", { willReadFrequently: true });
  if (!context) return undefined;
  context.drawImage(frame, 0, 0, rect.width, rect.height);
  return lumaGrid(context.getImageData(0, 0, rect.width, rect.height).data, rect.width, rect.height, cols, rows);
}

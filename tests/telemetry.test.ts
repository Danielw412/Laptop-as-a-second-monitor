import { it, expect } from "vitest";
import { intervalDelays, intervalStats } from "../viewer/src/telemetry";
it("preserves missing counters as unavailable rather than zero", () => {
  expect(intervalStats({time:2000}, {time:1000})).toEqual({loss:null,bitrate:null,fps:null,dropped:null});
});
it("does not invent a delta when a counter first appears", () => {
  expect(intervalStats({time:2000,decoded:60}, {time:1000})?.fps).toBeNull();
});
it("uses interval loss rather than cumulative session loss", () => {
  const a = {
    time: 1000,
    bytes: 1000,
    received: 100,
    lost: 20,
    decoded: 60,
    dropped: 3,
  };
  expect(
    intervalStats(
      { ...a, time: 2000, bytes: 2000, received: 200, decoded: 120 },
      a,
    ),
  ).toEqual({ loss: 0, bitrate: 8000, fps: 60, dropped: 0 });
});
it("resets on counter rollover", () =>
  expect(
    intervalStats(
      { time: 2000, bytes: 0, received: 0, lost: 0, decoded: 0, dropped: 0 },
      { time: 1000, bytes: 100, received: 1, lost: 0, decoded: 1, dropped: 0 },
    ),
  ).toBeUndefined());
it("reports per-frame receiver delays over the interval", () => {
  const a = { time: 1000, jitterBufferDelay: 1, jitterBufferEmitted: 100, decodeTime: 0.5, processingDelay: 2, decoded: 100, freezes: 1 };
  const b = { ...a, time: 2000, jitterBufferDelay: 1.6, jitterBufferEmitted: 160, decodeTime: 0.62, processingDelay: 2.9, decoded: 160, freezes: 1 };
  const d = intervalDelays(b, a)!;
  expect(d.jitterBufferMs).toBeCloseTo(10);
  expect(d.decodeMs).toBeCloseTo(2);
  expect(d.processingMs).toBeCloseTo(15);
  expect(d.freezes).toBe(0);
});
it("keeps receiver delays unavailable without both samples or counters", () => {
  expect(intervalDelays({ time: 2000 })).toBeUndefined();
  expect(intervalDelays({ time: 2000, decoded: 10 }, { time: 1000 })).toEqual({ jitterBufferMs: null, decodeMs: null, processingMs: null, freezes: null });
  expect(intervalDelays({ time: 2000, decodeTime: 1, decoded: 10 }, { time: 1000, decodeTime: 1, decoded: 10 })?.decodeMs).toBeNull();
});

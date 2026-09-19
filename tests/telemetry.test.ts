import { it, expect } from "vitest";
import { intervalActivity, intervalDelays, intervalQuality, intervalStats } from "../viewer/src/telemetry";
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

it("reports the mean quantizer over the interval, not the session", () => {
  const first = { qpSum: 1000, decoded: 50, received: 50, pli: 1, nack: 2, keyFrames: 1 };
  const q = intervalQuality({ qpSum: 1600, decoded: 70, received: 75, pli: 3, nack: 2, keyFrames: 2 }, first);
  expect(q.qp).toBe(30); // 600 over 20 frames, not 1600 over 70
  expect(q.corrupted).toBe(5);
  expect(q.pli).toBe(2);
  expect(q.nack).toBe(0);
  expect(q.keyFramesDecoded).toBe(1);
});
it("reports quality counters as unavailable rather than zero", () => {
  expect(intervalQuality({ qpSum: 100, decoded: 10 })).toEqual({
    qp: null, corrupted: null, pli: null, nack: null, keyFramesDecoded: null,
  });
  // A browser that does not publish qpSum must not look like a perfect picture.
  expect(intervalQuality({ decoded: 70 }, { decoded: 50 }).qp).toBeNull();
  // No frames decoded in the interval: a mean over nothing is not zero.
  expect(intervalQuality({ qpSum: 100, decoded: 50 }, { qpSum: 100, decoded: 50 }).qp).toBeNull();
});

const counters = {
  time: 1000, framesReceived: 100, framesDecoded: 100, framesRendered: 99, packetsReceived: 1000, packetsLost: 5,
  firCount: 0, freezesDuration: 0.5, pauseCount: 1, pausesDuration: 6, interFrameDelay: 1.6, squaredInterFrameDelay: 0.0256,
  jitterBufferTargetDelay: 0.2, jitterBufferEmitted: 100,
};
it("reports receiver frame and packet flow as interval deltas", () => {
  const next = {
    ...counters, time: 2000, framesReceived: 160, framesDecoded: 158, framesRendered: 157, packetsReceived: 1600,
    packetsLost: 7, firCount: 1, freezesDuration: 0.75, pauseCount: 1, pausesDuration: 6,
    // 58 frames: 57 gaps of 16 ms and one of 100 ms
    interFrameDelay: 1.6 + 57 * 0.016 + 0.1, squaredInterFrameDelay: 0.0256 + 57 * 0.016 ** 2 + 0.1 ** 2,
    jitterBufferTargetDelay: 0.2 + 58 * 0.002, jitterBufferEmitted: 158,
  };
  const a = intervalActivity(next, counters);
  expect(a.intervalMs).toBe(1000);
  expect(a.intervalFramesReceived).toBe(60);
  expect(a.intervalFramesDecoded).toBe(58);
  expect(a.intervalFramesRendered).toBe(58);
  expect(a.intervalPacketsReceived).toBe(600);
  expect(a.intervalPacketsLost).toBe(2);
  expect(a.fir).toBe(1);
  expect(a.freezeMs).toBeCloseTo(250);
  expect(a.pauses).toBe(0);
  expect(a.pauseMs).toBe(0);
  expect(a.interFrameDelayMs).toBeCloseTo(((57 * 0.016 + 0.1) / 58) * 1000);
  const mean = (57 * 0.016 + 0.1) / 58;
  expect(a.interFrameDelayStdMs).toBeCloseTo(Math.sqrt((57 * 0.016 ** 2 + 0.1 ** 2) / 58 - mean * mean) * 1000);
  expect(a.jitterBufferTargetMs).toBeCloseTo(2);
});
it("keeps receiver activity unavailable rather than zero", () => {
  // First sample, and a browser that publishes none of these counters.
  const none = intervalActivity(counters);
  expect(Object.values(none).every((v) => v === null)).toBe(true);
  const { intervalMs, ...counts } = intervalActivity({ time: 2000 }, { time: 1000 });
  expect(intervalMs).toBe(1000); // The interval itself is known even when no counter is
  expect(Object.values(counts).every((v) => v === null)).toBe(true);
  // A counter that appears only now has no interval yet.
  expect(intervalActivity({ time: 2000, firCount: 3 }, { time: 1000 }).fir).toBeNull();
  // No frames decoded: a mean inter-frame delay over nothing is unknown, not zero.
  const idle = intervalActivity({ ...counters, time: 2000 }, counters);
  expect(idle.intervalFramesDecoded).toBe(0);
  expect(idle.interFrameDelayMs).toBeNull();
  expect(idle.jitterBufferTargetMs).toBeNull();
  // Time did not advance: nothing to report.
  expect(intervalActivity({ ...counters }, counters).intervalFramesDecoded).toBeNull();
});
it("treats a counter that went down as a new stream, not as negative activity", () => {
  const reset = intervalActivity({ ...counters, time: 2000, framesReceived: 3, framesDecoded: 2, packetsReceived: 40, packetsLost: 0 }, counters);
  expect(reset.intervalFramesReceived).toBeNull();
  expect(reset.intervalFramesDecoded).toBeNull();
  expect(reset.intervalPacketsReceived).toBeNull();
  expect(reset.intervalPacketsLost).toBeNull();
  // packetsLost alone falling (a duplicate arrived) is no loss, not a reset.
  const duplicate = intervalActivity({ ...counters, time: 2000, packetsReceived: 1100, packetsLost: 4 }, counters);
  expect(duplicate.intervalPacketsLost).toBe(0);
  expect(duplicate.intervalPacketsReceived).toBe(100);
});

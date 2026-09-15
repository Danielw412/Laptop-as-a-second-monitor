import { it, expect } from "vitest";
import { intervalStats } from "../viewer/src/telemetry";
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

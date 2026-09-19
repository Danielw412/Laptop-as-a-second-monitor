import type { Telemetry } from "../../shared/protocol";
export type Previous = {
  time: number;
  bytes?: number;
  received?: number;
  lost?: number;
  decoded?: number;
  dropped?: number;
};
export function intervalStats(current: Previous, previous?: Previous) {
  if (
    !previous ||
    current.time <= previous.time ||
    (current.bytes !== undefined && previous.bytes !== undefined && current.bytes < previous.bytes)
  )
    return undefined;
  const seconds = (current.time - previous.time) / 1000;
  const difference = (a: number | undefined, b: number | undefined) => a === undefined || b === undefined ? null : Math.max(0, a-b);
  const received = difference(current.received, previous.received), lost = difference(current.lost, previous.lost);
  const bytes = difference(current.bytes, previous.bytes), decoded = difference(current.decoded, previous.decoded);
  return {
    loss: lost === null || received === null ? null : lost / Math.max(1, received + lost),
    bitrate: bytes === null ? null : bytes * 8 / seconds,
    fps: decoded === null ? null : decoded / seconds,
    dropped: difference(current.dropped, previous.dropped),
  };
}
/** Cumulative delay counters from inbound-rtp; each is a sum over frames plus its frame count. */
export type DelayCounters = {
  time: number;
  jitterBufferDelay?: number;
  jitterBufferEmitted?: number;
  decodeTime?: number;
  processingDelay?: number;
  decoded?: number;
  freezes?: number;
};
/** Cumulative picture-quality counters from inbound-rtp. */
export type QualityCounters = {
  qpSum?: number;
  decoded?: number;
  received?: number;
  pli?: number;
  nack?: number;
  keyFrames?: number;
};
/**
 * Quality over the interval: the mean quantizer the decoder saw (pixelation), and how many frames arrived but
 * never decoded (loss). Returns nulls rather than zeros where a counter is unavailable, so the host's log never
 * shows a clean picture that was really a missing statistic.
 */
export function intervalQuality(current: QualityCounters, previous?: QualityCounters) {
  const difference = (a: number | undefined, b: number | undefined) =>
    a === undefined || b === undefined ? null : Math.max(0, a - b);
  if (!previous) return { qp: null, corrupted: null, pli: null, nack: null, keyFramesDecoded: null };
  const frames = difference(current.decoded, previous.decoded);
  const quantizer = difference(current.qpSum, previous.qpSum);
  const received = difference(current.received, previous.received);
  return {
    qp: frames && quantizer !== null ? quantizer / frames : null,
    // Frames the decoder was handed but never produced: the visible tearing and smearing.
    corrupted: received === null || frames === null ? null : Math.max(0, received - frames),
    pli: difference(current.pli, previous.pli),
    nack: difference(current.nack, previous.nack),
    keyFramesDecoded: difference(current.keyFrames, previous.keyFrames),
  };
}
/** Per-frame receiver delays over the interval (ms), or null where a counter is unavailable. */
export function intervalDelays(current: DelayCounters, previous?: DelayCounters) {
  if (!previous || current.time <= previous.time) return undefined;
  const perFrame = (sum: number | undefined, prevSum: number | undefined, n: number | undefined, prevN: number | undefined) => {
    if (sum === undefined || prevSum === undefined || n === undefined || prevN === undefined) return null;
    const frames = n - prevN, seconds = sum - prevSum;
    return frames > 0 && seconds >= 0 ? (seconds * 1000) / frames : null;
  };
  return {
    jitterBufferMs: perFrame(current.jitterBufferDelay, previous.jitterBufferDelay, current.jitterBufferEmitted, previous.jitterBufferEmitted),
    decodeMs: perFrame(current.decodeTime, previous.decodeTime, current.decoded, previous.decoded),
    processingMs: perFrame(current.processingDelay, previous.processingDelay, current.decoded, previous.decoded),
    freezes: current.freezes === undefined || previous.freezes === undefined ? null : Math.max(0, current.freezes - previous.freezes),
  };
}
/** Cumulative inbound-rtp counters that only mean something as a change over the interval. Durations in seconds. */
export type ActivityCounters = {
  time: number;
  framesReceived?: number;
  framesDecoded?: number;
  framesRendered?: number;
  packetsReceived?: number;
  packetsLost?: number;
  firCount?: number;
  freezesDuration?: number;
  pauseCount?: number;
  pausesDuration?: number;
  interFrameDelay?: number;
  squaredInterFrameDelay?: number;
  jitterBufferTargetDelay?: number;
  jitterBufferEmitted?: number;
};
export type Activity = {
  /** How long the interval behind these counts was: the browser may sample slower than the page asks. */
  intervalMs: number | null;
  intervalFramesReceived: number | null;
  intervalFramesDecoded: number | null;
  intervalFramesRendered: number | null;
  intervalPacketsReceived: number | null;
  intervalPacketsLost: number | null;
  fir: number | null;
  freezeMs: number | null;
  pauses: number | null;
  pauseMs: number | null;
  interFrameDelayMs: number | null;
  interFrameDelayStdMs: number | null;
  jitterBufferTargetMs: number | null;
};
const NO_ACTIVITY: Activity = {
  intervalMs: null, intervalFramesReceived: null, intervalFramesDecoded: null, intervalFramesRendered: null,
  intervalPacketsReceived: null, intervalPacketsLost: null, fir: null, freezeMs: null, pauses: null, pauseMs: null,
  interFrameDelayMs: null, interFrameDelayStdMs: null, jitterBufferTargetMs: null,
};
/**
 * Frame and packet flow at the receiver over the interval, how long the picture was frozen or paused, and how evenly
 * frames were shown. A browser that does not publish a counter gets null, never zero; a counter that went down
 * belongs to a new stream, so that interval is unknown (null) rather than zero or negative.
 */
export function intervalActivity(current: ActivityCounters, previous?: ActivityCounters): Activity {
  if (!previous || current.time <= previous.time) return { ...NO_ACTIVITY };
  const delta = (a: number | undefined, b: number | undefined) =>
    a === undefined || b === undefined || a < b ? null : a - b;
  const ms = (seconds: number | null) => (seconds === null ? null : seconds * 1000);
  const decoded = delta(current.framesDecoded, previous.framesDecoded);
  const packets = delta(current.packetsReceived, previous.packetsReceived);
  // packetsLost is expected minus received, so duplicates can lower it: a fall is no loss, not a reset.
  const lost =
    current.packetsLost === undefined || previous.packetsLost === undefined || packets === null
      ? null
      : Math.max(0, current.packetsLost - previous.packetsLost);
  // Inter-frame delay mean and spread from the running sums, per the spec's variance formula.
  const sum = delta(current.interFrameDelay, previous.interFrameDelay);
  const squares = delta(current.squaredInterFrameDelay, previous.squaredInterFrameDelay);
  const mean = sum !== null && decoded ? sum / decoded : null;
  const spread =
    mean !== null && squares !== null && decoded ? Math.sqrt(Math.max(0, squares / decoded - mean * mean)) : null;
  const target = delta(current.jitterBufferTargetDelay, previous.jitterBufferTargetDelay);
  const emitted = delta(current.jitterBufferEmitted, previous.jitterBufferEmitted);
  return {
    intervalMs: current.time - previous.time,
    intervalFramesReceived: delta(current.framesReceived, previous.framesReceived),
    intervalFramesDecoded: decoded,
    intervalFramesRendered: delta(current.framesRendered, previous.framesRendered),
    intervalPacketsReceived: packets,
    intervalPacketsLost: lost,
    fir: delta(current.firCount, previous.firCount),
    freezeMs: ms(delta(current.freezesDuration, previous.freezesDuration)),
    pauses: delta(current.pauseCount, previous.pauseCount),
    pauseMs: ms(delta(current.pausesDuration, previous.pausesDuration)),
    interFrameDelayMs: ms(mean),
    interFrameDelayStdMs: ms(spread),
    jitterBufferTargetMs: target !== null && emitted ? (target * 1000) / emitted : null,
  };
}
export class ReceiverStats {
  private previous?: Previous;
  private previousDelays?: DelayCounters;
  private previousQuality?: QualityCounters;
  private previousActivity?: ActivityCounters;
  async sample(
    pc: RTCPeerConnection,
  ): Promise<
    { telemetry: Telemetry; diagnostics: Record<string, unknown> } | undefined
  > {
    const report = await pc.getStats();
    let video: RTCInboundRtpStreamStats | undefined;
    let pair: RTCIceCandidatePairStats | undefined;
    let selectedPairId: string | undefined;
    report.forEach((s) => {
      if (s.type === "inbound-rtp" && s.kind === "video") video = s;
      if (s.type === "transport" && s.selectedCandidatePairId)
        selectedPairId = s.selectedCandidatePairId;
    });
    if (selectedPairId) pair = report.get(selectedPairId);
    if (!video) return;
    const v = video as RTCInboundRtpStreamStats & {
      decoderImplementation?: string;
      powerEfficientDecoder?: boolean;
      framesPerSecond?: number;
      totalProcessingDelay?: number;
      freezeCount?: number;
      qpSum?: number;
      framesReceived?: number;
      keyFramesDecoded?: number;
    };
    const rttMs = pair?.currentRoundTripTime === undefined ? null : pair.currentRoundTripTime * 1000;
    const now = {
      time: v.timestamp,
      bytes: v.bytesReceived,
      received: v.packetsReceived,
      lost: v.packetsLost,
      decoded: v.framesDecoded,
      dropped: v.framesDropped,
    };
    const delta = intervalStats(now, this.previous);
    this.previous = now;
    const counters: DelayCounters = {
      time: v.timestamp,
      jitterBufferDelay: v.jitterBufferDelay,
      jitterBufferEmitted: v.jitterBufferEmittedCount,
      decodeTime: v.totalDecodeTime,
      processingDelay: v.totalProcessingDelay,
      decoded: v.framesDecoded,
      freezes: v.freezeCount,
    };
    const delays = intervalDelays(counters, this.previousDelays);
    this.previousDelays = counters;
    const quality: QualityCounters = {
      qpSum: v.qpSum,
      decoded: v.framesDecoded,
      received: v.framesReceived,
      pli: v.pliCount,
      nack: v.nackCount,
      keyFrames: v.keyFramesDecoded,
    };
    const picture = intervalQuality(quality, this.previousQuality);
    this.previousQuality = quality;
    const counts: ActivityCounters = {
      time: v.timestamp,
      framesReceived: v.framesReceived,
      framesDecoded: v.framesDecoded,
      framesRendered: v.framesRendered,
      packetsReceived: v.packetsReceived,
      packetsLost: v.packetsLost,
      firCount: v.firCount,
      freezesDuration: v.totalFreezesDuration,
      pauseCount: v.pauseCount,
      pausesDuration: v.totalPausesDuration,
      interFrameDelay: v.totalInterFrameDelay,
      squaredInterFrameDelay: v.totalSquaredInterFrameDelay,
      jitterBufferTargetDelay: v.jitterBufferTargetDelay,
      jitterBufferEmitted: v.jitterBufferEmittedCount,
    };
    const activity = intervalActivity(counts, this.previousActivity);
    this.previousActivity = counts;
    const telemetry: Telemetry = {
      type: "telemetry",
      loss: delta && v.packetsReceived !== undefined && v.packetsLost !== undefined ? delta.loss : null,
      bitrate: delta && v.bytesReceived !== undefined ? delta.bitrate : null,
      fps: delta && v.framesDecoded !== undefined ? delta.fps : null,
      dropped: delta && v.framesDropped !== undefined ? delta.dropped : null,
      rttMs,
      jitterMs: v.jitter === undefined ? null : v.jitter * 1000,
      decoded: v.framesDecoded ?? null,
      jitterBufferMs: delays?.jitterBufferMs ?? null,
      decodeMs: delays?.decodeMs ?? null,
      processingMs: delays?.processingMs ?? null,
      freezes: delays?.freezes ?? null,
      ...picture,
      ...activity,
      // Only when the selected candidate pair actually carries an estimate; many browsers publish it for the
      // sending side alone.
      availableIncomingBitrate:
        typeof pair?.availableIncomingBitrate === "number" ? pair.availableIncomingBitrate : null,
      // Which decoder ran (hardware or software) is often gated by the browser for privacy; null when withheld.
      decoder: typeof v.decoderImplementation === "string" ? v.decoderImplementation.slice(0, 64) : null,
      powerEfficientDecoder: typeof v.powerEfficientDecoder === "boolean" ? v.powerEfficientDecoder : null,
    };
    return {
      telemetry,
      diagnostics: {
        ...telemetry,
        type: "receiver-stats",
        framesDropped: v.framesDropped,
        codec: v.codecId ? report.get(v.codecId)?.mimeType : undefined,
        width: v.frameWidth,
        height: v.frameHeight,
        decoder: v.decoderImplementation,
        efficientDecoder: v.powerEfficientDecoder,
        nack: v.nackCount,
        pli: v.pliCount,
        // Session-cumulative jitter buffer delay; the interval value above is what the host receives.
        sessionJitterBufferMs: v.jitterBufferEmittedCount
          ? (1000 * (v.jitterBufferDelay ?? 0)) / v.jitterBufferEmittedCount
          : null,
        protocol: pair ? report.get(pair.localCandidateId)?.protocol : undefined,
        localCandidate: pair
          ? report.get(pair.localCandidateId)?.candidateType
          : undefined,
        remoteCandidate: pair
          ? report.get(pair.remoteCandidateId)?.candidateType
          : undefined,
      },
    };
  }
}

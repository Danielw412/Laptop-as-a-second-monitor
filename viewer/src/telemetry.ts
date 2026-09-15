import type { Telemetry } from "../../shared/protocol";
export type Previous = {
  time: number;
  bytes: number;
  received: number;
  lost: number;
  decoded: number;
  dropped: number;
};
export function intervalStats(current: Previous, previous?: Previous) {
  if (
    !previous ||
    current.time <= previous.time ||
    current.bytes < previous.bytes
  )
    return undefined;
  const seconds = (current.time - previous.time) / 1000;
  const received = Math.max(0, current.received - previous.received),
    lost = Math.max(0, current.lost - previous.lost);
  return {
    loss: lost / Math.max(1, received + lost),
    bitrate: ((current.bytes - previous.bytes) * 8) / seconds,
    fps: Math.max(0, current.decoded - previous.decoded) / seconds,
    dropped: Math.max(0, current.dropped - previous.dropped),
  };
}
export class ReceiverStats {
  private previous?: Previous;
  async sample(
    pc: RTCPeerConnection,
  ): Promise<
    { telemetry: Telemetry; diagnostics: Record<string, unknown> } | undefined
  > {
    const report = await pc.getStats();
    let video: RTCInboundRtpStreamStats | undefined;
    let rttMs = 0;
    let pair: RTCIceCandidatePairStats | undefined;
    report.forEach((s) => {
      if (s.type === "inbound-rtp" && s.kind === "video") video = s;
      if (s.type === "candidate-pair" && s.state === "succeeded" && s.nominated)
        pair = s;
    });
    if (!video) return;
    const v = video as RTCInboundRtpStreamStats & {
      decoderImplementation?: string;
      powerEfficientDecoder?: boolean;
      framesPerSecond?: number;
    };
    if (pair) rttMs = (pair.currentRoundTripTime ?? 0) * 1000;
    const now = {
      time: v.timestamp,
      bytes: v.bytesReceived ?? 0,
      received: v.packetsReceived ?? 0,
      lost: v.packetsLost ?? 0,
      decoded: v.framesDecoded ?? 0,
      dropped: v.framesDropped ?? 0,
    };
    const delta = intervalStats(now, this.previous);
    this.previous = now;
    if (!delta) return;
    const telemetry: Telemetry = {
      type: "telemetry",
      ...delta,
      rttMs,
      jitterMs: (v.jitter ?? 0) * 1000,
      decoded: now.decoded,
    };
    return {
      telemetry,
      diagnostics: {
        ...telemetry,
        width: v.frameWidth,
        height: v.frameHeight,
        decoder: v.decoderImplementation,
        efficientDecoder: v.powerEfficientDecoder,
        nack: v.nackCount,
        pli: v.pliCount,
        jitterBufferMs: v.jitterBufferEmittedCount
          ? (1000 * (v.jitterBufferDelay ?? 0)) / v.jitterBufferEmittedCount
          : 0,
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

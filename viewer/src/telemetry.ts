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
export class ReceiverStats {
  private previous?: Previous;
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
    const telemetry: Telemetry = {
      type: "telemetry",
      loss: delta && v.packetsReceived !== undefined && v.packetsLost !== undefined ? delta.loss : null,
      bitrate: delta && v.bytesReceived !== undefined ? delta.bitrate : null,
      fps: delta && v.framesDecoded !== undefined ? delta.fps : null,
      dropped: delta && v.framesDropped !== undefined ? delta.dropped : null,
      rttMs,
      jitterMs: v.jitter === undefined ? null : v.jitter * 1000,
      decoded: v.framesDecoded ?? null,
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
        jitterBufferMs: v.jitterBufferEmittedCount
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

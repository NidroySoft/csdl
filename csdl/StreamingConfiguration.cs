// csdl - a cross-platform libtorrent wrapper for .NET
// Licensed under Apache-2.0 - see the license file for more information

using System;
using csdl.Native;

namespace csdl;

/// <summary>
/// Fine‑tuning parameters for the embedded streaming server.
/// All properties have safe defaults suitable for most torrents.
/// </summary>
public class StreamingConfiguration
{
    /// <summary>
    /// Maximum RAM cache size for pieces, in MiB.
    /// </summary>
    public int CacheLimitMb { get; set; } = 512;

    /// <summary>
    /// Minimum number of pieces to keep ahead of the current playback head.
    /// </summary>
    public int MinReadahead { get; set; } = 24;

    /// <summary>
    /// Maximum number of pieces to keep ahead of the current playback head.
    /// </summary>
    public int MaxReadahead { get; set; } = 192;

    /// <summary>
    /// Number of pieces to keep behind the playback head.
    /// </summary>
    public int BackWindow { get; set; } = 6;

    /// <summary>
    /// Base deadline for the piece currently being served, in milliseconds.
    /// </summary>
    public int DeadlineBaseMs { get; set; } = 1200;

    /// <summary>
    /// Additional deadline step for each subsequent piece in the window.
    /// </summary>
    public int DeadlineStepMs { get; set; } = 800;

    /// <summary>
    /// Minimum interval between sliding window updates, in milliseconds.
    /// </summary>
    public int WindowUpdateThrottleMs { get; set; } = 250;

    /// <summary>
    /// Polling interval while waiting for a piece to be available, in milliseconds.
    /// </summary>
    public int PiecePollIntervalMs { get; set; } = 50;

    /// <summary>
    /// Maximum number of polling attempts for a regular piece.
    /// </summary>
    public int PiecePollMaxAttempts { get; set; } = 300;

    /// <summary>
    /// Maximum number of polling attempts for an anchor piece (first/last).
    /// </summary>
    public int AnchorPiecePollMaxAttempts { get; set; } = 1200;

    /// <summary>
    /// Number of consecutive piece timeouts before the stream is aborted.
    /// </summary>
    public int EnsurePieceMaxRetries { get; set; } = 3;

    /// <summary>
    /// Minimum interval between re‑emissions of a piece deadline, in milliseconds.
    /// </summary>
    public int DeadlineReemitIntervalMs { get; set; } = 2000;

    /// <summary>
    /// Number of pieces to buffer at the start before switching to streaming mode.
    /// </summary>
    public int StartupBufferPieces { get; set; } = 12;

    /// <summary>
    /// Number of tail pieces to keep as anchors (for quick seeking / metadata).
    /// </summary>
    public int TailPieces { get; set; } = 3;

    /// <summary>
    /// Whether to actively prefetch tail pieces during bootstrap.
    /// <c>true</c> (recommended) helps with quick seeks and metadata retrieval.
    /// </summary>
    public bool EnableTailPrefetch { get; set; } = true;

    /// <summary>
    /// Applies this configuration to the native streaming engine.
    /// </summary>
    internal void Apply()
    {
        NativeMethods.ConfigureStreamServer(
            CacheLimitMb,
            MinReadahead, MaxReadahead,
            BackWindow,
            DeadlineBaseMs, DeadlineStepMs,
            WindowUpdateThrottleMs,
            PiecePollIntervalMs,
            PiecePollMaxAttempts,
            AnchorPiecePollMaxAttempts,
            EnsurePieceMaxRetries,
            DeadlineReemitIntervalMs,
            StartupBufferPieces,
            TailPieces,
            EnableTailPrefetch);
    }
}
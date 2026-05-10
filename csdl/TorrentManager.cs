// csdl - a cross-platform libtorrent wrapper for .NET
// Licensed under Apache-2.0 - see the license file for more information

using csdl.Enums;
using csdl.Native;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace csdl;

public class TorrentManager
{
    private readonly string _savePath;
    internal readonly IntPtr TorrentSessionHandle;
    private bool _detached;
    private IReadOnlyList<TorrentManagerFile> _files;



    internal TorrentManager(IntPtr sessionHandle, IntPtr torrentSessionHandle, string savePath, TorrentInfo info)
    {
        _sessionHandle = sessionHandle;
        TorrentSessionHandle = torrentSessionHandle;
        _savePath = savePath;
        Info = info;
    }
    private readonly IntPtr _sessionHandle;
    private int? _pieceSize;

    /// <summary>
    /// Tamaño de pieza del torrent (en bytes). Se consulta una vez y se cachea.
    /// </summary>
    public int PieceSize
    {
        get
        {
            if (_pieceSize == null)
            {
                ObjectDisposedException.ThrowIf(_detached, this);
                _pieceSize = NativeMethods.GetPieceLength(TorrentSessionHandle);
                if (_pieceSize <= 0)
                    throw new InvalidOperationException("No se pudo obtener el tamaño de pieza del torrent. ¿Metadatos aún no disponibles?");
            }
            return _pieceSize.Value;
        }
    }

    /// <summary>
    /// Information about the .torrent file
    /// </summary>
    public TorrentInfo Info { get; }

    /// <summary>
    /// Information about the files contained within the torrent, with additional
    /// properties including file priorities and target save paths.
    /// </summary>
    public IReadOnlyList<TorrentManagerFile> Files
        => _files ??= Info.Files
            .Select(x => new TorrentManagerFile(TorrentSessionHandle, _savePath, x))
            .ToList();

    /// <summary>
    /// Gets the current status of the torrent.
    /// </summary>
    /// <remarks>
    /// Every time this is called, an unmanaged call is made to the underlying system.
    /// Where possible, cache the result of this method to avoid unnecessary overhead.
    /// </remarks>
    public TorrentStatus GetCurrentStatus()
    {
        ObjectDisposedException.ThrowIf(_detached, this);
        NativeMethods.GetTorrentStatus(TorrentSessionHandle, out var status);
        return status;
    }

    /// <summary>
    /// Starts or resumes the torrent.
    /// </summary>
    public void Start()
    {
        ObjectDisposedException.ThrowIf(_detached, this);
        NativeMethods.StartTorrent(TorrentSessionHandle);
    }

    /// <summary>
    /// Stops the torrent.
    /// </summary>
    public void Stop()
    {
        ObjectDisposedException.ThrowIf(_detached, this);
        NativeMethods.StopTorrent(TorrentSessionHandle);
    }

    /// <summary>
    /// Reannounces the torrent to all trackers.
    /// </summary>
    public void ReannounceAllTrackers(TimeSpan interval, bool force = false)
    {
        if (interval.Seconds <= -1)
            throw new ArgumentOutOfRangeException(nameof(interval), "Interval must be a positive value.");

        ObjectDisposedException.ThrowIf(_detached, this);
        NativeMethods.ReannounceTorrent(TorrentSessionHandle, (int)interval.TotalSeconds, force);
    }

    // ── Streaming ─────────────────────────────────────────────────────────────

    /// <summary>
    /// Starts the embedded HTTP streaming server for a specific file in this torrent.
    /// </summary>
    /// <param name="fileIndex">Index of the file to stream.</param>
    /// <param name="port">Port to bind on. Pass 0 to auto-assign.</param>
    /// <returns>
    /// The streaming base URL, e.g. <c>"http://127.0.0.1:55126/"</c>.
    /// Pass this directly to LibVLC or any HTTP-capable media player.
    /// </returns>
    /// <exception cref="InvalidOperationException">
    /// Thrown if the native server fails to start (torrent has no metadata yet,
    /// invalid file index, or port already in use).
    /// </exception>
    public string StartStreaming(int fileIndex, int port = 55126)
    {
        ObjectDisposedException.ThrowIf(_detached, this);

        // Parada suave previa
        if (NativeMethods.IsStreamServerRunning())
            NativeMethods.StopStreamServer();

        IntPtr urlPtr = NativeMethods.StartStreamServer(_sessionHandle, TorrentSessionHandle, fileIndex, port);

        // Si falló por estado corrupto, hacemos reset completo y reintentamos una sola vez
        if (urlPtr == IntPtr.Zero)
        {
            NativeMethods.ResetStreamServer();           // Limpieza profunda
            urlPtr = NativeMethods.StartStreamServer(_sessionHandle, TorrentSessionHandle, fileIndex, port);
        }

        if (urlPtr == IntPtr.Zero)
            throw new InvalidOperationException(
                "Failed to start embedded streaming server. " +
                "Ensure the torrent has metadata and the file index is valid.");

        return Marshal.PtrToStringUTF8(urlPtr)
            ?? throw new InvalidOperationException("Streaming server returned an empty URL.");
    }

    /// <summary>
    /// Stops the embedded streaming server if it is running.
    /// </summary>
    public void StopStreaming()
    {
        if (NativeMethods.IsStreamServerRunning())
            NativeMethods.StopStreamServer();
    }

    public string? GetLastStreamingError()
    {
        IntPtr ptr = NativeMethods.GetLastStreamError();
        if (ptr == IntPtr.Zero) return null;
        return Marshal.PtrToStringUTF8(ptr);
    }

    /// <summary>
    /// Returns true if the embedded streaming server is currently running.
    /// </summary>
    public bool IsStreaming => NativeMethods.IsStreamServerRunning();

    // ── Internal ──────────────────────────────────────────────────────────────

    internal void MarkAsDetached()
    {
        _detached = true;
    }

    // ── TorrentManagerFile ────────────────────────────────────────────────────

    public class TorrentManagerFile
    {
        private readonly IntPtr _torrentSessionHandle;

        internal TorrentManagerFile(IntPtr torrentSessionHandle, string savePath, TorrentFileInfo info)
        {
            _torrentSessionHandle = torrentSessionHandle;
            Info = info;
            Path = System.IO.Path.IsPathRooted(Info.Path)
                ? Info.Path
                : System.IO.Path.Combine(savePath, Info.Path);
        }

        /// <summary>
        /// File information as provided by the .torrent file.
        /// </summary>
        public TorrentFileInfo Info { get; }

        /// <summary>
        /// The full resolved path to the file on disk.
        /// </summary>
        public string Path { get; }

        /// <summary>
        /// The download priority of this file.
        /// </summary>
        public FileDownloadPriority Priority
        {
            get => NativeMethods.GetFilePriority(_torrentSessionHandle, Info.Index);
            set => NativeMethods.SetFilePriority(_torrentSessionHandle, Info.Index, value);
        }
    }

    // ── Seek inteligente ─────────────────────────────────────────────────────
    public bool IsByteAvailable(int fileIndex, long bytePosition)
    {
        ObjectDisposedException.ThrowIf(_detached, this);
        return NativeMethods.IsByteAvailable(TorrentSessionHandle, fileIndex, bytePosition);
    }

    public bool PrioritizeSeekRange(int fileIndex, long bytePosition)
    {
        ObjectDisposedException.ThrowIf(_detached, this);
        return NativeMethods.PrioritizeSeekRange(TorrentSessionHandle, fileIndex, bytePosition, PieceSize);
    }

    /// <summary>
    /// Espera asíncronamente hasta que el byte en <paramref name="bytePosition"/> esté disponible o se agote el tiempo.
    /// </summary>
    /// <param name="fileIndex">Índice del archivo dentro del torrent.</param>
    /// <param name="bytePosition">Posición absoluta dentro del archivo.</param>
    /// <param name="timeout">Tiempo máximo de espera.</param>
    /// <param name="cancellationToken">Token de cancelación.</param>
    /// <returns><c>true</c> si el byte ya está disponible; <c>false</c> si se agotó el tiempo o se canceló.</returns>
    public async Task<bool> WaitForByteAvailableAsync(int fileIndex, long bytePosition, TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        cts.CancelAfter(timeout);

        while (!cts.IsCancellationRequested)
        {
            if (IsByteAvailable(fileIndex, bytePosition))
                return true;

            await Task.Delay(200, cancellationToken).ConfigureAwait(false);
        }
        return false;
    }

    /// <summary>
    /// Inicia el servidor de streaming para el archivo más grande del torrent
    /// (normalmente el vídeo). Usa el puerto 55201.
    /// </summary>
    /// <returns>La URL de streaming.</returns>
    public string StartStreamingLargestFile(int port = 55201)
    {
        var largest = Info.Files.OrderByDescending(f => f.FileSize).First();
        return StartStreaming(largest.Index, port);
    }

}
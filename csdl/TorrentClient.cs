// csdl - a cross-platform libtorrent wrapper for .NET
// Licensed under Apache-2.0 - see the license file for more information

using csdl.Alerts;
using csdl.Enums;
using csdl.Native;
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace csdl;

/// <summary>
/// Represents a client that can register and control torrents download/uploads
/// </summary>
public class TorrentClient : IDisposable
{
    internal const AlertCategories RequiredAlertCategories = AlertCategories.Status;

    private readonly ConcurrentDictionary<string, TorrentManager> _attachedManagers = new(StringComparer.OrdinalIgnoreCase);

    // need to keep a reference to the delegate to prevent GC invalidating it
    private readonly NativeMethods.SessionEventCallback _eventCallback;
    private readonly IntPtr _handle;

    private bool _disposed;
    private bool _includeUnmappedEvents;

    /// <summary>
    /// Creates a new instance of <see cref="TorrentClient"/> with default settings.
    /// </summary>
    public TorrentClient() : this(new SettingsPack())
    {
    }

    /// <summary>
    /// Creates a new instance of <see cref="TorrentClient"/> with the provided configuration.
    /// </summary>
    public TorrentClient(TorrentClientConfig config) : this(config.Build())
    {
    }

    /// <summary>
    /// Creates a new instance of <see cref="TorrentClient"/> with the provided settings pack (advanced usage).
    /// </summary>
    public unsafe TorrentClient(SettingsPack pack)
    {
        ValidateSettingsPack(pack);

        var packHandle = pack.BuildNative();

        try
        {
            _handle = NativeMethods.CreateSession(packHandle.ToPointer());

            if (_handle == IntPtr.Zero)
            {
                throw new InvalidOperationException("Failed to create session.");
            }

            _eventCallback = ProxyRaisedEvent;
            NativeMethods.SetEventCallback(_handle, _eventCallback, true);
        }
        finally
        {
            NativeMethods.FreeSettingsPack(packHandle);
        }
    }

    ~TorrentClient()
    {
        Dispose();
    }

    /// <summary>
    /// Gets the active torrents currently attached to the session.
    /// </summary>
    public IEnumerable<TorrentManager> ActiveTorrents => _attachedManagers.Values;

    /// <summary>
    /// Whether to include events that only produce a <see cref="SessionAlert"/> with no additional data.
    /// </summary>
    /// <remarks>
    /// Changing this value after subscribing will cause the event callback to be reset.
    /// </remarks>
    public bool IncludeUnmappedEvents
    {
        get => _includeUnmappedEvents;
        set
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            _includeUnmappedEvents = value;

            // reset event callback if set
            if (_eventCallback != null)
            {
                NativeMethods.ClearEventCallback(_handle);
                NativeMethods.SetEventCallback(_handle, _eventCallback, value);
            }
        }
    }

    /// <summary>
    /// Gets or sets the default path to save downloaded torrents to.
    /// If a torrent is attached with a relative save path and this property is set, the save path will be combined with this property.
    /// </summary>
    public string DefaultDownloadPath { get; set; } = Path.Combine(Environment.CurrentDirectory, "downloads");

    /// <summary>
    /// Event invoked when a session alert is raised.
    /// The underlying event collection system is unmanaged, and is started/shutdown on the first/last subscription to this event.
    /// </summary>
    public event EventHandler<SessionAlert> AlertRaised;

    /// <summary>
    /// Applies a settings pack to the session, updating the current configuration at some point in the future.
    /// </summary>
    public void UpdateSettings(SettingsPack pack)
    {
        ValidateSettingsPack(pack);

        var packHandle = pack.BuildNative();

        try
        {
            NativeMethods.ApplySettingsPack(_handle, packHandle);
        }
        finally
        {
            NativeMethods.FreeSettingsPack(packHandle);
        }
    }

    /// <summary>
    /// Attaches a torrent to the session, allowing it to be downloaded/uploaded.
    /// </summary>
    /// <param name="torrent">The <see cref="TorrentInfo"/> to attach</param>
    /// <param name="savePath">The path to save/read data from</param>
    /// <returns>A <see cref="TorrentManager"/> allowing the torrent to be controlled.</returns>
    /// <exception cref="InvalidOperationException">The torrent was unable to be attached to the underlying session</exception>
    public TorrentManager AttachTorrent(TorrentInfo torrent, string savePath = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (_attachedManagers.ContainsKey(torrent.Metadata.InfoHash))
        {
            throw new InvalidOperationException("Torrent is already attached to this session.");
        }

        savePath ??= DefaultDownloadPath;

        // relative paths will be combined with the default download path
        if (!Path.IsPathRooted(savePath))
        {
            savePath = Path.Combine(DefaultDownloadPath, savePath);
        }

        // ensure the save path exists
        if (!Directory.Exists(savePath))
        {
            Directory.CreateDirectory(savePath);
        }

        var handle = NativeMethods.AttachTorrent(_handle, torrent.InfoHandle, Path.GetFullPath(savePath));

        if (handle == IntPtr.Zero)
        {
            throw new InvalidOperationException("Failed to attach torrent to session.");
        }

        var manager = new TorrentManager(_handle, handle, savePath, torrent);
        _attachedManagers.TryAdd(manager.Info.Metadata.InfoHash, manager);

        return manager;
    }

    /// <summary>
    /// Detaches a torrent from the session, stopping any ongoing transfers.
    /// </summary>
    /// <param name="manager">The manager to detach</param>
    public void DetachTorrent(TorrentManager manager)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        // the manager is fully removed via the alert callback (fires once the torrent is fully removed)
        if (!_attachedManagers.ContainsKey(manager.Info.Metadata.InfoHash))
        {
            throw new InvalidOperationException("Unable to detach torrent from session. Ensure the torrent is attached to this session.");
        }

        manager.Stop();
        NativeMethods.DetachTorrent(_handle, manager.TorrentSessionHandle);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        foreach (var session in ActiveTorrents)
        {
            try
            {
                DetachTorrent(session);
            }
            catch
            {
                // ignore
            }
        }

        _disposed = true;

        NativeMethods.ClearEventCallback(_handle);
        NativeMethods.FreeSession(_handle);

        GC.SuppressFinalize(this);
    }

    /// <summary>
    /// Performs a validation check on the current settings pack, updating any values to values required by this library to function
    /// </summary>
    private static void ValidateSettingsPack(SettingsPack settingsPack)
    {
        // ensure the alert mask is set to include the required categories
        settingsPack.Set("alert_mask", settingsPack.Get<int>("alert_mask").GetValueOrDefault(0) | (int)RequiredAlertCategories);
    }

    /// <summary>
    /// Marshals raised unmanaged events to managed equivalents, and forwards them to the <see cref="AlertRaised"/> event.
    /// These events are raised and proxied by the unmanaged library, and are automatically destroyed once the callback returns.
    /// </summary>
    /// <param name="eventPtr">A <see cref="IntPtr"/> to the underlying event (see <c>event.h</c>)</param>
    private unsafe void ProxyRaisedEvent(IntPtr eventPtr)
    {
        if (eventPtr == IntPtr.Zero)
        {
            return;
        }

        SessionAlert forwardAlert = null;
        switch ((AlertType)(*(int*)eventPtr.ToPointer()))
        {
            case AlertType.Generic:
            {
                var genericAlert = Marshal.PtrToStructure<NativeEvents.AlertBase>(eventPtr);
                forwardAlert = new SessionAlert(genericAlert);
                break;
            }

            case AlertType.TorrentStatus:
            {
                var statusAlert = Marshal.PtrToStructure<NativeEvents.TorrentStatusAlert>(eventPtr);
                if (!_attachedManagers.TryGetValue(Convert.ToHexString(statusAlert.info_hash), out var torrentSubject))
                {
                    return;
                }

                forwardAlert = new TorrentStatusAlert(statusAlert, torrentSubject);
                break;
            }

            case AlertType.ClientPerformance:
            {
                var performanceAlert = Marshal.PtrToStructure<NativeEvents.PerformanceWarningAlert>(eventPtr);
                forwardAlert = new PerformanceWarningAlert(performanceAlert);
                break;
            }

            case AlertType.Peer:
            {
                var peerAlert = Marshal.PtrToStructure<NativeEvents.PeerAlert>(eventPtr);
                if (!_attachedManagers.TryGetValue(Convert.ToHexString(peerAlert.info_hash), out var peerSubject))
                {
                    return;
                }

                forwardAlert = new PeerAlert(peerAlert, peerSubject);
                break;
            }

            case AlertType.TorrentRemoved:
            {
                var removedAlert = Marshal.PtrToStructure<NativeEvents.TorrentRemovedAlert>(eventPtr);
                if (!_attachedManagers.TryRemove(Convert.ToHexString(removedAlert.info_hash), out var manager))
                {
                    return;
                }

                // mark as detached to prevent further usage
                manager.MarkAsDetached();

                forwardAlert = new TorrentRemovedAlert(removedAlert, manager);
                break;
            }
        }

        if (forwardAlert == null)
        {
            return;
        }

        // the native library always invokes this from another thread
        AlertRaised?.Invoke(this, forwardAlert);


    }
  
    /// <summary>
    /// Applies streaming configuration to the session.
    /// Must be called before <see cref="TorrentManager.StartStreaming"/>.
    /// </summary>
    /// <param name="config">The streaming configuration to apply. If <c>null</c>, defaults are used.</param>
    public void ConfigureStreaming(StreamingConfiguration config)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        config ??= new StreamingConfiguration(); // safe defaults
        config.Apply();
    }

    /// <summary>
    /// Temporarily removes the managed alert callback so that the embedded streaming
    /// server can consume alerts internally. The managed AlertRaised event will stop
    /// firing until the callback is re-established via a settings change or a new session.
    /// </summary>
    public void ClearManagedAlertCallback()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        NativeMethods.ClearEventCallback(_handle);
    }

    /// <summary>
    /// Crea un <see cref="TorrentClient"/> preconfigurado para streaming,
    /// con todos los ajustes nativos necesarios.
    /// </summary>
    /// <param name="downloadPath">Directorio donde se guardarán los archivos.</param>
    /// <param name="streamingConfig">
    /// Configuración opcional del motor de streaming; si es <c>null</c>, se usan valores
    /// de producción (256 MiB de caché, prefetch de cola, etc.).
    /// </param>
    public static TorrentClient CreateForStreaming(
        string downloadPath,
        StreamingConfiguration? streamingConfig = null)
    {
        // ⚠️ Orden obligatorio: el pack DEBE contener las categorías de alertas
        // y los ajustes nativos para que el motor funcione.
        var pack = new SettingsPack();

        // Alertas
        pack.Set("alert_mask", (int)(
            AlertCategories.Status |
            AlertCategories.Storage |
            AlertCategories.Error |
            AlertCategories.PieceProgress));

        // Conexiones y red (réplica de create_streaming_settings)
        pack.Set("connections_limit", 300);
        pack.Set("connection_speed", 200);
        pack.Set("torrent_connect_boost", 100);
        pack.Set("min_reconnect_time", 1);
        pack.Set("peer_connect_timeout", 3);
        pack.Set("request_timeout", 10);
        pack.Set("piece_timeout", 10);
        pack.Set("peer_timeout", 10);
        pack.Set("urlseed_timeout", 10);
        pack.Set("max_out_request_queue", 2500);
        pack.Set("max_allowed_in_request_queue", 2500);
        pack.Set("request_queue_time", 1);
        pack.Set("whole_pieces_threshold", 0);
        pack.Set("smooth_connects", false);

        // I/O de disco
        pack.Set("disk_io_read_mode", 1);   // enable_os_cache
        pack.Set("disk_io_write_mode", 1);
        pack.Set("max_queued_disk_bytes", 128 * 1024 * 1024);
        pack.Set("aio_threads", 8);
        pack.Set("file_pool_size", 60);
        pack.Set("strict_end_game_mode", false);

        // DHT, LSD, UPnP, NAT-PMP
        pack.Set("enable_dht", true);
        pack.Set("enable_lsd", true);
        pack.Set("enable_upnp", true);
        pack.Set("enable_natpmp", true);

        var client = new TorrentClient(pack);
        client.DefaultDownloadPath = downloadPath;

        // Aplicar configuración fina del motor (la de siempre)
        client.ConfigureStreaming(streamingConfig ?? new StreamingConfiguration());

        return client;
    }

    // Dentro de TorrentClient.cs, junto al resto de métodos públicos

    /// <summary>
    /// Adjunta un torrent, configura la descarga para el archivo más grande,
    /// inicia el servidor de streaming y arranca la descarga, todo en un solo paso.
    /// </summary>
    /// <param name="torrent">Torrent a adjuntar.</param>
    /// <param name="port">Puerto del servidor de streaming (por defecto 55201).</param>
    /// <param name="savePath">Directorio de descarga; si es null, se usa <see cref="DefaultDownloadPath"/>.</param>
    /// <returns>URL del streaming lista para abrir en un reproductor.</returns>
    public string QuickStream(TorrentInfo torrent, int port = 55201, string? savePath = null)
    {
        // 1. Elegir el archivo más grande (vídeo)
        var largest = torrent.Files.OrderByDescending(f => f.FileSize).First();
        int index = largest.Index;

        // 2. Adjuntar torrent y priorizar solo el archivo de vídeo
        var manager = AttachTorrent(torrent, savePath ?? DefaultDownloadPath);
        foreach (var f in manager.Files)
            f.Priority = f.Info.Index == index
                ? FileDownloadPriority.High
                : FileDownloadPriority.DoNotDownload;

        // 3. Liberar el callback gestionado (orden obligatorio #1)
        ClearManagedAlertCallback();

        // 4. Iniciar servidor de streaming (orden obligatorio #2)
        string url = manager.StartStreaming(index, port);

        // 5. Iniciar la descarga (orden obligatorio #3)
        manager.Start();

        return url;
    }
}
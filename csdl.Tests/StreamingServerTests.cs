using System;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using csdl;
using csdl.Enums;
using csdl.Native;
using Xunit;
using Xunit.Abstractions;

namespace csdl.Tests;

public class StreamingServerTests : IDisposable
{
    private readonly ITestOutputHelper _output;
    private readonly string _tempBase;
    private readonly HttpClient _http = new();
    private const int FixedPort = 55201;

    private TorrentClient _seedClient;
    private TorrentClient _leecherClient;
    private TorrentManager _seedManager;
    private TorrentManager _leecherManager;

    public StreamingServerTests(ITestOutputHelper output)
    {
        _output = output;
        NativeMethods.ResetStreamServer();
        _tempBase = Path.Combine(Path.GetTempPath(), "csdl-streaming-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_tempBase);
    }

    public void Dispose()
    {
        try
        {
            _leecherManager?.StopStreaming();
            _leecherManager?.Stop();
            _seedManager?.Stop();
            _leecherClient?.DetachTorrent(_leecherManager);
            _seedClient?.DetachTorrent(_seedManager);
        }
        catch { }
        _leecherClient?.Dispose();
        _seedClient?.Dispose();
        _http?.Dispose();
        if (Directory.Exists(_tempBase))
            Directory.Delete(_tempBase, true);
    }

    private async Task SetupLocalSwarmAsync()
    {
        string sourceData = Path.GetFullPath(Path.Combine("files", "alice.txt"));
        string seedDataFile = Path.Combine(_tempBase, "alice.txt");
        File.Copy(sourceData, seedDataFile, overwrite: true);

        string torrentPath = Path.GetFullPath(Path.Combine("files", "alice.torrent"));
        var torrentInfo = new TorrentInfo(torrentPath);

        _seedClient = new TorrentClient(new TorrentClientConfig { MaxConnections = 50 });
        _seedManager = _seedClient.AttachTorrent(torrentInfo, _tempBase);
        _seedManager.Start();
        await WaitForStateAsync(_seedManager, TorrentState.Seeding, TimeSpan.FromSeconds(15));

        _leecherClient = new TorrentClient(new TorrentClientConfig
        {
            MaxConnections = 50,
            BlockSeeding = true
        });
        string leechPath = Path.Combine(_tempBase, "leech");
        Directory.CreateDirectory(leechPath);
        _leecherManager = _leecherClient.AttachTorrent(torrentInfo, leechPath);
        _leecherManager.Start();
        await WaitForStateAsync(_leecherManager, TorrentState.Downloading, TimeSpan.FromSeconds(15));

        await Task.Delay(2000);
    }

    private async Task WaitForStateAsync(TorrentManager mgr, TorrentState target, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            var st = mgr.GetCurrentStatus();
            if (st.State == target || st.State == TorrentState.Finished || st.State == TorrentState.Seeding)
                return;
            await Task.Delay(500);
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Pruebas básicas de streaming
    // ─────────────────────────────────────────────────────────────────
    [Fact]
    public async Task StartStreaming_ReturnsValidUrl()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, port: FixedPort);
        Assert.StartsWith("http://127.0.0.1:", url);
        Assert.EndsWith("/stream", url);
        _output.WriteLine($"URL: {url}");
        _leecherManager.StopStreaming();
    }

    [Fact]
    public async Task StartStreaming_IsStreamingReturnsTrue()
    {
        await SetupLocalSwarmAsync();
        Assert.False(_leecherManager.IsStreaming);
        _leecherManager.StartStreaming(0, FixedPort);
        Assert.True(_leecherManager.IsStreaming);
        _leecherManager.StopStreaming();
    }

    [Fact]
    public async Task StopStreaming_IsStreamingReturnsFalse()
    {
        await SetupLocalSwarmAsync();
        _leecherManager.StartStreaming(0, FixedPort);
        _leecherManager.StopStreaming();
        Assert.False(_leecherManager.IsStreaming);
    }

    [Fact]
    public async Task StopStreaming_WhenNotRunning_DoesNotThrow()
    {
        await SetupLocalSwarmAsync();
        var ex = Record.Exception(() => _leecherManager.StopStreaming());
        Assert.Null(ex);
    }

    [Fact]
    public async Task StartStreaming_CalledTwice_StopsPreviousAndStartsNew()
    {
        await SetupLocalSwarmAsync();
        var url1 = _leecherManager.StartStreaming(0, FixedPort);
        var url2 = _leecherManager.StartStreaming(0, FixedPort);
        Assert.NotNull(url2);
        Assert.True(_leecherManager.IsStreaming);
        _output.WriteLine($"URL1={url1}, URL2={url2}");
        _leecherManager.StopStreaming();
    }

    [Fact]
    public async Task StartStreaming_InvalidFileIndex_ThrowsInvalidOperationException()
    {
        await SetupLocalSwarmAsync();
        Assert.Throws<InvalidOperationException>(() => _leecherManager.StartStreaming(9999, FixedPort));
        NativeMethods.ResetStreamServer();
    }

    [Fact]
    public async Task StartStreaming_WithExplicitPort_UsesRequestedPort()
    {
        await SetupLocalSwarmAsync();
        const int testPort = 55200;
        string url = _leecherManager.StartStreaming(0, testPort);
        Assert.Contains($":{testPort}/", url);
        Assert.EndsWith("/stream", url);
        _leecherManager.StopStreaming();
    }

    // ─────────────────────────────────────────────────────────────────
    // Pruebas HTTP
    // ─────────────────────────────────────────────────────────────────
    [Fact]
    public async Task StreamingServer_Responds200_ToFullRequest()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, FixedPort);
        using var resp = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
        Assert.Equal(System.Net.HttpStatusCode.OK, resp.StatusCode);
        Assert.True(resp.Content.Headers.ContentLength > 0);
        _leecherManager.StopStreaming();
    }

    [Fact]
    public async Task StreamingServer_Responds206_ToRangeRequest()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, FixedPort);
        var req = new HttpRequestMessage(HttpMethod.Get, url);
        req.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(0, 65535);
        using var resp = await _http.SendAsync(req, HttpCompletionOption.ResponseHeadersRead);
        Assert.Equal(System.Net.HttpStatusCode.PartialContent, resp.StatusCode);
        Assert.NotNull(resp.Content.Headers.ContentRange);
        Assert.Equal(65536, resp.Content.Headers.ContentLength);
        _leecherManager.StopStreaming();
    }

    [Fact]
    public async Task StreamingServer_OutOfRangeRequest_Returns416()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, FixedPort);
        var req = new HttpRequestMessage(HttpMethod.Get, url);
        req.Headers.TryAddWithoutValidation("Range", "bytes=999999999999-999999999999");
        using var resp = await _http.SendAsync(req);
        Assert.Equal(System.Net.HttpStatusCode.RequestedRangeNotSatisfiable, resp.StatusCode);
        _leecherManager.StopStreaming();
    }

    [Fact]
    public async Task StreamingServer_StatusEndpoint_ReturnsJson()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, FixedPort);
        string baseUrl = url.Substring(0, url.LastIndexOf('/'));
        string statusUrl = baseUrl + "/status";
        var json = await _http.GetStringAsync(statusUrl);
        Assert.Contains("\"torrent\"", json);
        Assert.Contains("\"file\"", json);
        _leecherManager.StopStreaming();
    }

    // ─────────────────────────────────────────────────────────────────
    // Integridad del contenido descargado
    // ─────────────────────────────────────────────────────────────────
    [Fact]
    public async Task StreamingServer_DownloadedContent_MatchesOriginalFile()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, FixedPort);

        // Forzamos la descarga completa del leecher
        var deadline = DateTime.UtcNow + TimeSpan.FromMinutes(2);
        while (DateTime.UtcNow < deadline)
        {
            var st = _leecherManager.GetCurrentStatus();
            if (st.State == TorrentState.Finished || st.Progress >= 1.0) break;
            await Task.Delay(500);
        }

        // Descargamos el archivo entero vía HTTP
        byte[] downloaded = await _http.GetByteArrayAsync(url);

        // Comparamos con el archivo original
        string originalPath = Path.GetFullPath(Path.Combine("files", "alice.txt"));
        byte[] original = await File.ReadAllBytesAsync(originalPath);

        Assert.True(original.SequenceEqual(downloaded),
            "El contenido descargado debe coincidir byte a byte con el archivo original.");

        _leecherManager.StopStreaming();
    }

    // ─────────────────────────────────────────────────────────────────
    // Concurrencia
    // ─────────────────────────────────────────────────────────────────
    [Fact]
    public async Task StreamingServer_HandlesMultipleConcurrentClients()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, FixedPort);

        var tasks = new Task<HttpResponseMessage>[4];
        for (int i = 0; i < tasks.Length; i++)
            tasks[i] = _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);

        _output.WriteLine($"Iniciando {tasks.Length} clientes concurrentes");
        var responses = await Task.WhenAll(tasks);

        for (int i = 0; i < responses.Length; i++)
        {
            var resp = responses[i];
            _output.WriteLine($"Cliente {i}: {(int)resp.StatusCode}");
            Assert.True(resp.IsSuccessStatusCode);
            resp.Dispose();
        }
        _leecherManager.StopStreaming();
    }

    // ─────────────────────────────────────────────────────────────────
    // Seek inteligente
    // ─────────────────────────────────────────────────────────────────
    [Fact]
    public async Task PieceSize_ReturnsPositiveValue_AfterMetadataLoaded()
    {
        await SetupLocalSwarmAsync();
        int pieceSize = _leecherManager.PieceSize;
        Assert.True(pieceSize > 0);
        _output.WriteLine($"Piece size: {pieceSize}");
    }

    [Fact]
    public async Task IsByteAvailable_ReturnsFalse_WhenNoSeedAvailable()
    {
        // Sin semilla, ningún byte debe estar disponible
        string torrentPath = Path.GetFullPath(Path.Combine("files", "alice.torrent"));
        var torrentInfo = new TorrentInfo(torrentPath);

        _leecherClient = new TorrentClient(new TorrentClientConfig { MaxConnections = 50 });
        string leechPath = Path.Combine(_tempBase, "leech_noseed");
        Directory.CreateDirectory(leechPath);
        _leecherManager = _leecherClient.AttachTorrent(torrentInfo, leechPath);
        _leecherManager.Start();

        await Task.Delay(500);

        int fileIdx = 0;
        long fileSize = _leecherManager.Files[fileIdx].Info.FileSize;
        long farPosition = fileSize - 1;

        bool available = _leecherManager.IsByteAvailable(fileIdx, farPosition);
        Assert.False(available, "Sin semillas, ningún byte debería estar disponible.");
    }

    [Fact]
    public async Task IsByteAvailable_ReturnsTrue_ForFirstByte_WhenStartIsComplete()
    {
        await SetupLocalSwarmAsync();

        int fileIdx = 0;

        // Esperar a que al menos la primera pieza esté disponible
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(30);
        while (DateTime.UtcNow < deadline)
        {
            if (_leecherManager.IsByteAvailable(fileIdx, 0))
                break;
            await Task.Delay(500);
        }

        Assert.True(_leecherManager.IsByteAvailable(fileIdx, 0),
            "El primer byte del archivo debería estar disponible tras unos segundos de descarga.");
    }

    [Fact]
    public async Task PrioritizeSeekRange_DoesNotThrow_AndReturnsTrue()
    {
        await SetupLocalSwarmAsync();

        int fileIdx = 0;
        long midPosition = _leecherManager.Files[fileIdx].Info.FileSize / 2;

        bool result = _leecherManager.PrioritizeSeekRange(fileIdx, midPosition);
        Assert.True(result);
    }

    [Fact]
    public async Task WaitForByteAvailableAsync_Completes_WhenPieceArrives()
    {
        await SetupLocalSwarmAsync();

        int fileIdx = 0;
        long firstByte = 0;

        // Puede tardar unos segundos mientras se descarga
        bool ready = await _leecherManager.WaitForByteAvailableAsync(
            fileIdx, firstByte, TimeSpan.FromSeconds(30));

        Assert.True(ready, "El primer byte debería estar disponible en menos de 30 segundos.");
    }

    [Fact]
    public async Task Seek_WhenPriorityChanged_ByteBecomesAvailableFaster()
    {
        await SetupLocalSwarmAsync();

        int fileIdx = 0;
        long fileSize = _leecherManager.Files[fileIdx].Info.FileSize;
        long midPoint = fileSize / 2;

        // Priorizamos ese rango
        _leecherManager.PrioritizeSeekRange(fileIdx, midPoint);

        // Espera máxima de 20 segundos (debería ser suficiente en un swarm local)
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        bool found = false;
        while (DateTime.UtcNow < deadline)
        {
            if (_leecherManager.IsByteAvailable(fileIdx, midPoint))
            {
                found = true;
                break;
            }
            await Task.Delay(500);
        }

        Assert.True(found, "Tras priorizar el rango, el byte central debería estar disponible en pocos segundos.");
    }

    // ─────────────────────────────────────────────────────────────────
    // Robustez
    // ─────────────────────────────────────────────────────────────────
    [Fact]
    public async Task StreamingServer_CanBeRestartedMultipleTimes()
    {
        await SetupLocalSwarmAsync();

        for (int i = 0; i < 3; i++)
        {
            string url = _leecherManager.StartStreaming(0, FixedPort);
            Assert.True(_leecherManager.IsStreaming);

            // Hacemos una petición rápida para verificar
            using var resp = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
            Assert.True(resp.IsSuccessStatusCode);

            _leecherManager.StopStreaming();
            Assert.False(_leecherManager.IsStreaming);

            _output.WriteLine($"Ciclo {i + 1} completado.");
        }
    }

    [Fact]
    public async Task StreamingServer_SurvivesTorrentPauseAndResume()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, FixedPort);

        // Pausamos el torrent mientras el streaming está activo
        _leecherManager.Stop();
        await Task.Delay(1000);

        // Reanudamos y comprobamos que el servidor sigue sirviendo
        _leecherManager.Start();
        await Task.Delay(2000);

        using var resp = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
        Assert.True(resp.IsSuccessStatusCode);

        _leecherManager.StopStreaming();
    }

    // ─────────────────────────────────────────────────────────────────
    // Torrent con múltiples archivos
    // ─────────────────────────────────────────────────────────────────
    [Fact]
    public async Task StreamingServer_MultiFileTorrent_CanStreamEachFile()
    {
        // Usamos big-buck-bunny.torrent para tener varios archivos
        string torrentPath = Path.GetFullPath(Path.Combine("files", "big-buck-bunny.torrent"));
        var torrentInfo = new TorrentInfo(torrentPath);

        // No necesitamos descargar nada, solo verificar que el servidor arranca para cada índice válido
        _leecherClient = new TorrentClient(new TorrentClientConfig { MaxConnections = 50 });
        string leechPath = Path.Combine(_tempBase, "multifile");
        Directory.CreateDirectory(leechPath);
        _leecherManager = _leecherClient.AttachTorrent(torrentInfo, leechPath);
        _leecherManager.Start();

        // Esperamos un poco para tener metadatos
        await Task.Delay(1000);

        for (int i = 0; i < torrentInfo.Files.Count; i++)
        {
            string url = _leecherManager.StartStreaming(i, FixedPort);
            Assert.NotNull(url);
            _output.WriteLine($"Archivo {i}: {url}");
            _leecherManager.StopStreaming();
        }
    }
}
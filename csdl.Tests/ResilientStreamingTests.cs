using csdl;
using csdl.Enums;
using csdl.Native;
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using Xunit;
using Xunit.Abstractions;

namespace csdl.Tests;

public class ResilientStreamingTests : IDisposable
{
    private readonly ITestOutputHelper _output;
    private readonly string _tempBase;
    private readonly HttpClient _http = new();
    private const int FixedPort = 55201;


    private TorrentClient _seedClient;
    private TorrentClient _leecherClient;
    private TorrentManager _seedManager;
    private TorrentManager _leecherManager;

    public ResilientStreamingTests(ITestOutputHelper output)
    {
        _output = output;
        NativeMethods.ResetStreamServer();   // ← limpieza total antes de cada test
        _tempBase = Path.Combine(Path.GetTempPath(), "csdl-resilient-" + Guid.NewGuid().ToString("N"));
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

    private void Log(string message) =>
        _output.WriteLine($"[{DateTime.Now:HH:mm:ss.fff}] {message}");

    private void LogStatus(string prefix, TorrentManager mgr)
    {
        var st = mgr.GetCurrentStatus();
        Log($"{prefix} | State={st.State} | " +
            $"Progress={(st.Progress * 100):F2}% | " +
            $"Down={st.DownloadRate / 1024.0:F1} KB/s | " +
            $"Up={st.UploadRate / 1024.0:F1} KB/s | " +
            $"Peers={st.PeerCount} | Seeds={st.SeedCount}");
    }

    private void LogMetrics(string title, Stopwatch sw, TorrentManager mgr)
    {
        long fileSize = mgr.Files[0].Info.FileSize;
        double seconds = sw.Elapsed.TotalSeconds;
        double speedKBps = seconds > 0 ? (fileSize / 1024.0) / seconds : 0;
        var status = mgr.GetCurrentStatus();

        Log($"=== {title} ===");
        Log($"Tiempo total:        {seconds:F2} s");
        Log($"Tamaño archivo:      {fileSize} bytes ({fileSize / 1024.0:F1} KB)");
        Log($"Velocidad media:     {speedKBps:F2} KB/s");
        Log($"Estado final:        {status.State}");
        Log($"Progreso:            {status.Progress * 100:F2}%");
        Log($"==============================");
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

    private async Task SetupLocalSwarmAsync()
    {
        Log("=== SETUP SWARM ===");
        Log($"Temp folder: {_tempBase}");

        string sourceData = Path.GetFullPath(Path.Combine("files", "alice.txt"));
        string seedDataFile = Path.Combine(_tempBase, "alice.txt");
        File.Copy(sourceData, seedDataFile, overwrite: true);

        string torrentPath = Path.GetFullPath(Path.Combine("files", "alice.torrent"));
        var torrentInfo = new TorrentInfo(torrentPath);

        _seedClient = new TorrentClient(new TorrentClientConfig { MaxConnections = 50 });
        _seedManager = _seedClient.AttachTorrent(torrentInfo, _tempBase);
        _seedManager.Start();
        Log("Seed iniciado");
        await WaitForStateAsync(_seedManager, TorrentState.Seeding, TimeSpan.FromSeconds(15));
        LogStatus("Seed listo", _seedManager);

        _leecherClient = new TorrentClient(new TorrentClientConfig { MaxConnections = 50, BlockSeeding = true });
        string leechPath = Path.Combine(_tempBase, "leech");
        Directory.CreateDirectory(leechPath);
        _leecherManager = _leecherClient.AttachTorrent(torrentInfo, leechPath);
        _leecherManager.Start();
        Log("Leecher iniciado");
        await WaitForStateAsync(_leecherManager, TorrentState.Downloading, TimeSpan.FromSeconds(15));
        LogStatus("Leecher descargando", _leecherManager);

        await Task.Delay(2000);
    }

    [Fact]
    public async Task DownloadCompletes_WithLocalSeed()
    {
        await SetupLocalSwarmAsync();

        Stopwatch sw = Stopwatch.StartNew();
        var deadline = DateTime.UtcNow + TimeSpan.FromMinutes(2);

        while (DateTime.UtcNow < deadline)
        {
            var st = _leecherManager.GetCurrentStatus();
            LogStatus("DESCARGANDO", _leecherManager);

            if (st.State == TorrentState.Finished || st.Progress >= 1.0)
            {
                Log("Descarga completada");
                break;
            }

            await Task.Delay(1000);
        }

        sw.Stop();
        Assert.True(_leecherManager.GetCurrentStatus().Progress >= 1.0);
        LogMetrics("DESCARGA COMPLETA", sw, _leecherManager);
    }

    [Fact]
    public async Task Streaming_Resilient_ToSlowPieceArrival()
    {
        await SetupLocalSwarmAsync();
        int fileIdx = 0;

        Stopwatch sw = Stopwatch.StartNew();
        string url = _leecherManager.StartStreaming(fileIdx, port: FixedPort);
        Assert.NotNull(url);
        Log($"Streaming iniciado: {url}");

        _leecherManager.Stop();
        Log("Leecher detenido (simulando fallo)");
        await Task.Delay(2000);

        _leecherManager.Start();
        Log("Leecher reanudado");

        var response = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
        Assert.True(response.IsSuccessStatusCode);
        Log("HTTP streaming conectado");

        var ms = new MemoryStream();
        var stream = await response.Content.ReadAsStreamAsync();
        byte[] buffer = new byte[8192];
        int totalRead = 0;

        while (true)
        {
            int read = await stream.ReadAsync(buffer, 0, buffer.Length);
            if (read == 0) break;

            totalRead += read;
            ms.Write(buffer, 0, read);

            Log($"Streaming chunk: {read} bytes | Total: {totalRead}");
            LogStatus("STREAM", _leecherManager);
        }

        byte[] downloaded = ms.ToArray();
        sw.Stop();

        byte[] expected = await File.ReadAllBytesAsync(Path.GetFullPath(Path.Combine("files", "alice.txt")));
        Assert.True(expected.SequenceEqual(downloaded));

        _leecherManager.StopStreaming();
        LogMetrics("RESILIENCIA – Pausa y reanudación", sw, _leecherManager);
    }

    [Fact]
    public async Task StatusEndpoint_Resilient_DuringNetworkIssues()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, FixedPort);
        string baseUrl = url.Substring(0, url.LastIndexOf('/'));
        string statusUrl = baseUrl + "/status";

        _leecherManager.Stop();
        await Task.Delay(500);
        var json = await _http.GetStringAsync(statusUrl);
        Log($"Consultando status: {statusUrl}");
        Log($"Respuesta (offline): {json}");
        Assert.Contains("\"torrent\"", json);

        _leecherManager.Start();
        await Task.Delay(3000);
        json = await _http.GetStringAsync(statusUrl);
        Log($"Respuesta (online): {json}");
        Assert.Contains("\"progress\"", json);

        _leecherManager.StopStreaming();
    }

    [Fact]
    public async Task MultipleClients_Streaming_SameFile()
    {
        await SetupLocalSwarmAsync();
        string url = _leecherManager.StartStreaming(0, FixedPort);

        var tasks = new Task<HttpResponseMessage>[4];
        for (int i = 0; i < tasks.Length; i++)
            tasks[i] = _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);

        Log($"Iniciando {tasks.Length} clientes concurrentes");
        var responses = await Task.WhenAll(tasks);

        for (int i = 0; i < responses.Length; i++)
        {
            var resp = responses[i];
            Log($"Cliente {i}: {(int)resp.StatusCode}");
            Assert.True(resp.IsSuccessStatusCode);
            resp.Dispose();
        }
        _leecherManager.StopStreaming();
    }
}
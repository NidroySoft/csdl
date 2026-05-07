// csdl - a cross-platform libtorrent wrapper for .NET
// Licensed under Apache-2.0 - see the license file for more information

using System;
using System.IO;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using csdl;
using csdl.Enums;
using csdl.Native;
using Xunit;
using Xunit.Abstractions;

namespace csdl.Tests;

public class TorrentClientTests : IDisposable
{
    private readonly ITestOutputHelper _output;
    private readonly string _tempBase;
    private readonly HttpClient _http = new();
    private const int FixedPort = 55201;

    private TorrentClient _seedClient;
    private TorrentClient _leecherClient;
    private TorrentManager _seedManager;
    private TorrentManager _leecherManager;

    public TorrentClientTests(ITestOutputHelper output)
    {
        _output = output;
        NativeMethods.ResetStreamServer();
        _tempBase = Path.Combine(Path.GetTempPath(), "csdl-client-test-" + Guid.NewGuid().ToString("N"));
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

    [Fact]
    public async Task TestStreamingServer()
    {
        await SetupLocalSwarmAsync();

        int fileIndex = 0;
        string url = _leecherManager.StartStreaming(fileIndex, port: FixedPort);
        _output.WriteLine($"Streaming URL: {url}");

        Assert.NotNull(url);
        Assert.StartsWith("http://127.0.0.1:", url);
        Assert.True(_leecherManager.IsStreaming);

        string baseUrl = url.Substring(0, url.LastIndexOf('/'));
        var statusJson = await _http.GetStringAsync(baseUrl + "/status");
        _output.WriteLine($"Status JSON: {statusJson}");
        Assert.Contains("\"file\":", statusJson);
        Assert.Contains("\"index\": " + fileIndex, statusJson);
        Assert.Contains("\"name\": \"alice.txt\"", statusJson);

        _leecherManager.StopStreaming();
        Assert.False(_leecherManager.IsStreaming);
    }
}
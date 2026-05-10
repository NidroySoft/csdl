# csdl

Cross-platform .NET wrapper for [libtorrent](https://libtorrent.org) with an embedded HTTP streaming server.  
Designed for high-performance torrent downloads and low-latency media playback on Windows, Linux, macOS and Android.

[![Latest Nuget](https://img.shields.io/nuget/v/csdl?label=csdl&logo=nuget)](https://nuget.org/packages/csdl)
[![Android Nuget](https://img.shields.io/nuget/v/csdl.Native.android?label=csdl.Native.android&logo=nuget)](https://nuget.org/packages/csdl.Native.android)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](license.md)

---

## Table of Contents

- [Installation](#installation)
- [Quick Start](#quick-start)
- [Streaming](#streaming)
  - [One-line setup](#one-line-setup)
  - [Manual setup](#manual-setup)
  - [Startup order](#startup-order)
  - [Monitoring with /status](#monitoring-with-status)
  - [Seeking](#seeking)
  - [Configuration](#configuration)
- [Torrent management](#torrent-management)
- [Events](#events)
- [Supported platforms](#supported-platforms)
- [License](#license)

---

## Installation

```bash
dotnet add package csdl
```

For Android, also add the platform native package:

```bash
dotnet add package csdl.Native.android
```

---

## Quick Start

```csharp
using var client = new TorrentClient();

var torrent = new TorrentInfo("path/to/file.torrent");
var manager = client.AttachTorrent(torrent, "./downloads");

manager.Start();

// Poll until finished
while (manager.GetCurrentStatus().State is not (TorrentState.Finished or TorrentState.Seeding))
    await Task.Delay(1000);

manager.Stop();
client.DetachTorrent(manager);
```

---

## Streaming

The embedded HTTP server serves any file inside a torrent while it is still downloading.  
It manages its own piece scheduler, RAM cache, and playback window — no configuration required to get started.

### One-line setup

`CreateForStreaming` builds a `TorrentClient` with all native settings pre-tuned for streaming.  
`QuickStream` attaches the torrent, selects the largest file, starts the server and begins the download in a single call.

```csharp
using var client = TorrentClient.CreateForStreaming("./downloads");

var torrent = new TorrentInfo("big-buck-bunny.torrent");
string url = client.QuickStream(torrent);

// Open url in VLC, LibVLC, or any HTTP-capable media player
Console.WriteLine(url);
Console.ReadLine();
```

`QuickStream` accepts optional `port` and `savePath` parameters:

```csharp
string url = client.QuickStream(torrent, port: 8080, savePath: "./movies");
```

### Manual setup

Use the lower-level API when you need to control file priorities, monitor progress, or stream a specific file index.

```csharp
// 1. Create a streaming-optimised client
using var client = TorrentClient.CreateForStreaming("./downloads", new StreamingConfiguration
{
    CacheLimitMb       = 512,
    EnableTailPrefetch = true
});

// 2. Attach torrent and set file priorities
var torrent = new TorrentInfo("film.torrent");
var manager = client.AttachTorrent(torrent, "./downloads");

int videoIndex = manager.Files
    .OrderByDescending(f => f.Info.FileSize)
    .First().Info.Index;

foreach (var f in manager.Files)
    f.Priority = f.Info.Index == videoIndex
        ? FileDownloadPriority.High
        : FileDownloadPriority.DoNotDownload;

// 3. Hand alert ownership to the native streaming engine
client.ClearManagedAlertCallback();

// 4. Start the streaming server
string url = manager.StartStreaming(videoIndex, port: 55201);

// 5. Start the download
manager.Start();

Console.WriteLine($"Streaming at: {url}");
Console.ReadLine();

manager.StopStreaming();
manager.Stop();
```

### Startup order

> ⚠️ This order is mandatory when using the low-level API. `QuickStream` enforces it automatically.

| Step | Call |
|------|------|
| 1 | `AttachTorrent` |
| 2 | `ClearManagedAlertCallback` |
| 3 | `StartStreaming` |
| 4 | `manager.Start()` |

`ClearManagedAlertCallback` gives the native alert pump exclusive ownership of `pop_alerts`.  
If the managed callback remains active, it competes with the streaming engine for piece notifications and the server stalls.

### Monitoring with /status

The embedded server exposes a `/status` endpoint that returns a live JSON snapshot of the engine state.  
Use it to build loading indicators or debug monitors without calling into libtorrent from the managed side.

**Example response:**

```json
{
  "state": "streaming",
  "read_head": 42,
  "last_window_start": 36,
  "last_window_end": 234,
  "is_running": true,
  "cache": {
    "first_piece_cached": true,
    "last_piece_cached": true,
    "cached_in_window": 18,
    "window_size": 199
  },
  "inflight": {
    "inflight_count": 3,
    "inflight_pieces": [43, 44, 1053]
  },
  "torrent": {
    "progress": 0.35,
    "download_rate": 1250000,
    "num_peers": 45,
    "num_seeds": 12,
    "is_finished": false
  },
  "file": {
    "index": 0,
    "name": "Big Buck Bunny.mp4",
    "size": 276134946,
    "downloaded": 96647231,
    "progress": 0.35,
    "first_piece": 0,
    "last_piece": 1053
  }
}
```

**Polling /status from C#:**

```csharp
using var http = new HttpClient();

// Wait until the engine has left Bootstrap (file.progress >= 2%)
while (true)
{
    await Task.Delay(500);
    string json = await http.GetStringAsync($"http://127.0.0.1:{port}/status");
    using var doc = JsonDocument.Parse(json);
    double fileProgress = doc.RootElement
        .GetProperty("file").GetProperty("progress").GetDouble();

    if (fileProgress >= 0.02) break;
}

// Safe to open in player now
```

> **Tip:** do not call `IsByteAvailable` in a tight loop while streaming is active —  
> it calls `query_pieces` internally which interferes with the native alert pump.  
> Use `/status` instead.

### Seeking

When the user scrubs to a new position, tell the engine to prioritise that area before resuming playback:

```csharp
long targetByte = (long)(sliderValue * file.Info.FileSize);

player.Pause();
manager.PrioritizeSeekRange(fileIndex, targetByte);

bool ready = await manager.WaitForByteAvailableAsync(
    fileIndex, targetByte, TimeSpan.FromSeconds(10));

player.Seek(targetByte);
player.Play();
```

| Method | Description |
|--------|-------------|
| `IsByteAvailable(fileIndex, bytePosition)` | Returns true if the piece containing the byte is in cache. |
| `PrioritizeSeekRange(fileIndex, bytePosition)` | Shifts the download window to focus on the seek target. |
| `WaitForByteAvailableAsync(fileIndex, bytePosition, timeout)` | Waits asynchronously until the byte is available or the timeout expires. |

### Configuration

Pass a `StreamingConfiguration` to `CreateForStreaming` or `ConfigureStreaming` to tune the engine.  
All parameters have production-ready defaults — only adjust what you need.

```csharp
var config = new StreamingConfiguration
{
    CacheLimitMb               = 1024,  // RAM cache size in MB (default: 256)
    EnableTailPrefetch         = true,  // Pre-fetch last pieces for instant duration detection
    StartupBufferPieces        = 10,    // Pieces to buffer before entering streaming mode
    MinReadahead               = 4,     // Minimum readahead window (pieces)
    MaxReadahead               = 200,   // Maximum readahead window (pieces)
    PiecePollMaxAttempts       = 300,   // Max retries waiting for a piece
    DeadlineBaseMs             = 1000,  // Base urgency deadline (ms)
    DeadlineStepMs             = 500,   // Per-piece deadline increment (ms)
};

using var client = TorrentClient.CreateForStreaming("./downloads", config);
```

`ConfigureStreaming` can also be called on an existing client **before** `StartStreaming`:

```csharp
client.ConfigureStreaming(new StreamingConfiguration { CacheLimitMb = 512 });
```

---

## Torrent management

```csharp
// Attach with a custom save path
var manager = client.AttachTorrent(torrent, "./downloads/movies");

// File-level priority control
foreach (var file in manager.Files)
    file.Priority = FileDownloadPriority.DoNotDownload;

manager.Files[0].Priority = FileDownloadPriority.High;

// Status polling
var status = manager.GetCurrentStatus();
Console.WriteLine($"{status.Progress * 100:F1}% at {status.DownloadRate / 1024} KB/s");

// Re-announce to all trackers immediately
manager.ReannounceAllTrackers(TimeSpan.Zero, force: true);

// Stop and detach
manager.Stop();
client.DetachTorrent(manager);
```

### TorrentManagerFile

Each entry in `manager.Files` exposes:

| Member | Type | Description |
|--------|------|-------------|
| `Info` | `TorrentFileInfo` | File metadata from the .torrent (index, size, path, offset) |
| `Path` | `string` | Fully resolved path on disk |
| `Priority` | `FileDownloadPriority` | Get or set the download priority |

---

## Events

Subscribe to `AlertRaised` to receive status, peer, and performance notifications.

> **Note:** `AlertRaised` stops firing after `ClearManagedAlertCallback()` is called.  
> Restore it with `UpdateSettings` or by creating a new session if you need events during streaming.

```csharp
client.AlertRaised += (_, alert) =>
{
    switch (alert)
    {
        case TorrentStatusAlert status:
            Console.WriteLine($"State changed: {status.NewState}");
            break;
        case PeerAlert peer:
            Console.WriteLine($"Peer {peer.Type}: {peer.Address}");
            break;
        case PerformanceWarningAlert perf:
            Console.WriteLine($"Performance warning: {perf.WarningType}");
            break;
    }
};
```

Set `client.IncludeUnmappedEvents = true` to also receive generic `SessionAlert` objects for alert types that have no dedicated managed type.

---

## Supported platforms

| Platform | Architecture | Package |
|----------|-------------|---------|
| Windows  | x64, arm64  | `csdl` |
| Linux    | x64, arm64  | `csdl` |
| macOS    | x64, arm64  | `csdl` |
| Android  | arm64-v8a, armeabi-v7a, x86_64 | `csdl` + `csdl.Native.android` |

---

## License

Licensed under [Apache-2.0](license.md).
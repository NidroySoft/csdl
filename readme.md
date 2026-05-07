

```markdown
# csdl
Providing libtorrent through a C++ library for use in .NET on Windows, Linux, macOS and Android.

## Usage
[![Latest Nuget](https://img.shields.io/nuget/v/csdl.Native?label=csdl&logo=nuget)](https://nuget.org/packages/csdl)

Add `csdl` to the project via [NuGet](https://nuget.org/packages/csdl) and create a single `TorrentClient` instance.
This will usually be `static` (or singleton if using a dependency container), and a `TorrentClientConfig` can be passed in the constructor to configure the client.

```csharp
// create a new TorrentClient instance, optionally passing in the configuration
var options = new TorrentClientConfig
{
    ForceEncryption = true,
    MaxConnections = 500
};

using var client = new TorrentClient(options);

// bonus: there is also an event handler that can be subscribed to if more information is wanted.
client.AlertRaised += (sender, args) =>
{
    // args can be checked against all classes in the csdl.Alerts namespace for more properties.
    Console.WriteLine(args.Message);
};
```

.torrent files can be parsed either by passing in a file path or a byte array containing the file contents to the `TorrentInfo` class, and the instance will be populated accordingly.
Some metadata can be accessed from the `TorrentInfo` instance, such as the name of the torrent and a list of files contained within it.

```csharp
var filePath = "path/to/torrent/file.torrent";
var torrentInfo = new TorrentInfo(filePath);

// get the name and list of files
Console.WriteLine($"Name: {torrentInfo.Name}");
Console.WriteLine("Files:");

foreach (var file in torrentInfo.Files)
{
    Console.WriteLine($"- {file.Path} ({file.Size} bytes)");
}
```

After parsing a torrent file, it can be "attached" to the client to start downloading the files. Note this method will not start a download, but will prepare the client to download the files when `Start` is called.
This method also has an overload allowing a custom save path to be specified. If not, the default save path will be used (`client.DefaultDownloadPath`).

```csharp
// this will be saved to DefaultDownloadPath.
var torrentManager = client.AttachTorrent(torrentInfo);

// the metadata can still be accessed but files have additional properties including their final destination and their download priority, which can be changed.
torrentManager.Files[0].Priority = TorrentFilePriority.DoNotDownload;

// after setting priorities, the download can begin (or resume if the torrent was previously started)
torrentManager.Start();

// if we want a progress update, we can request one
var progress = torrentManager.GetCurrentStatus();

if (progress.State == TorrentState.Finished)
{
    torrentManager.Stop();

    // when we want to "dispose" the manager, we can detach it from the client
    client.DetachTorrent(torrentManager);
}
```

If we want to wait for the download to complete, a timer and a `TaskCompletionSource` can be used to await the completion of the download.

```csharp
async Task PerformDownload(TorrentClient client, TorrentInfo info, string savePath = null)
{
    var torrentTransfer = new TaskCompletionSource();
    var torrentManager = client.AttachTorrent(info, savePath);

    torrentManager.Start();

    using (new Timer(PerformProgressCheck, null, TimeSpan.Zero, TimeSpan.FromSeconds(1)))
    {
        await torrentTransfer.Task;
    }

    // at this point, the torrentManager has either finished downloading or seeding, and the poll has been stopped.

    client.DetachTorrent(torrentManager);
    return;

    // this function polls the progress (makes a call to libtorrent) every second to check if the torrent has finished downloading
    void PerformProgressCheck(object state)
    {
        if (torrentManager.GetCurrentStatus().State is TorrentState.Seeding or TorrentState.Finished)
        {
            torrentTransfer.SetResult();
        }
    }
}
```

## Streaming & Seek

`csdl` includes an embedded HTTP server that can serve any file inside a torrent directly to media players like VLC or LibVLCSharp.  
The server is automatically tuned for low‑latency streaming, using sparse storage, aggressive peer timeouts, and intelligent piece deadlines.

### Starting the streaming server

```csharp
// Attach a torrent (you may already have it)
var manager = client.AttachTorrent(torrentInfo);

// Start streaming the first file on port 0 (auto‑assign)
string url = manager.StartStreaming(fileIndex: 0, port: 0);

Console.WriteLine($"Stream available at: {url}");

// Open the URL in any HTTP‑capable media player.
```

- The returned URL already contains the `/stream` route (e.g. `http://127.0.0.1:55201/stream`).
- An additional `/status` endpoint returns a JSON object with download progress, peer counts, and piece availability.

### Intelligent seeking

When a user skips to a new position, the torrent engine needs to fetch the corresponding pieces before playback can continue.  
`csdl` exposes two low‑level helpers that allow you to build a seamless seek experience:

- **`IsByteAvailable(int fileIndex, long bytePosition)`** – checks whether the piece containing a specific byte has already been downloaded.
- **`PrioritizeSeekRange(int fileIndex, long bytePosition)`** – tells the download engine to focus all bandwidth on the pieces that cover the requested byte.

A typical seek flow from the UI layer looks like this:

```csharp
// User drags the progress bar to a new position
long targetByte = (long)(sliderValue * fileSizeInBytes);

// 1. Pause playback and tell the engine to prioritise the new point
player.Pause();
manager.PrioritizeSeekRange(fileIndex, targetByte);

// 2. Wait until the required piece is available (with a timeout)
bool ready = await manager.WaitForByteAvailableAsync(
    fileIndex, targetByte, TimeSpan.FromSeconds(10));

if (ready)
{
    // 3. Seek the player and resume
    player.Seek(TimeSpan.FromSeconds(targetByte / byteRate));
    player.Play();
}
else
{
    // Inform the user and resume from the previous position
    player.Play();
}
```

### Advanced configuration

`csdl` provides a built‑in `SettingsPack` factory for streaming‑optimised defaults. You can use it directly or modify it before creating the `TorrentClient`.

```csharp
var pack = new SettingsPack();
pack.Set("alert_mask", (int)AlertCategories.All);

// Apply streaming‑tuned settings
var streamingPack = NativeMethods.CreateStreamingSettings();  // returns an IntPtr
var client = new TorrentClient(new TorrentClientConfig
{
    // ...
});
```

> ⚠️ `NativeMethods.CreateStreamingSettings()` is a low‑level binding. In most cases the default `TorrentClient` constructor already applies sensible streaming defaults.

## Supported Systems

The native libraries, `csdl.Native`, are currently built for Windows, macOS and Linux for both x64 and arm64 architectures.

Android support is also provided by an optional package, [csdl.Native.android](https://nuget.org/packages/csdl.Native.android) which can be installed alongside `csdl` to extend platform compatibility to Android 5.0+ devices with `x86_64`, `armeabi-v7a` and `arm64-v8a` ABIs.

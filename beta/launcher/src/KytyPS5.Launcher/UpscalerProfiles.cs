using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;

namespace KytyPS5;

// Identifies each library game's upscaler interface when it is added or found (kyty-upscaler-scan: offline, cached by
// path and size/mtime, the game is never run). The runtime selector reads the same profiles at launch, so a game the
// launcher has not scanned yet is scanned there; this only makes the first launch fast and shows the result.
internal static class UpscalerProfiles
{
    public sealed record Status(string State, string Line, string? Profile, bool RuntimeVerified);

    public static string Scanner => Environment.GetEnvironmentVariable("KYTYPS5_UPSCALER_SCANNER")
        ?? Path.Combine(AppContext.BaseDirectory, "../tools/upscaler-scan/kyty-upscaler-scan");

    private static readonly ConcurrentDictionary<string, Status> Results = new(StringComparer.Ordinal);
    private static readonly ConcurrentDictionary<string, byte> Queued = new(StringComparer.Ordinal);
    private static readonly SemaphoreSlim OneAtATime = new(1, 1); // a .zar scan decompresses the executable: one at a time

    // Raised (on a worker thread) with the game path when its status arrives.
    public static event Action<string>? Changed;

    public static Status? Get(string gamePath) => Results.TryGetValue(gamePath, out var s) ? s : null;

    public static void ScanInBackground(IEnumerable<Game> games)
    {
        if (!File.Exists(Scanner)) return;
        foreach (var game in games)
        {
            if (!Queued.TryAdd(game.Path, 0)) continue;
            _ = Task.Run(async () =>
            {
                await OneAtATime.WaitAsync();
                try
                {
                    var status = Run(game.Path);
                    if (status is not null)
                    {
                        Results[game.Path] = status;
                        Changed?.Invoke(game.Path);
                    }
                }
                finally
                {
                    OneAtATime.Release();
                    Queued.TryRemove(game.Path, out _); // a later library refresh re-checks (cached unless the game changed)
                }
            });
        }
    }

    private static Status? Run(string gamePath)
    {
        try
        {
            var info = new ProcessStartInfo("python3") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            foreach (var a in new[] { "-I", Scanner, "status", gamePath }) info.ArgumentList.Add(a);
            using var process = Process.Start(info);
            if (process is null) return null;
            var stdout = process.StandardOutput.ReadToEndAsync();
            _ = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(TimeSpan.FromMinutes(5)))
            {
                try { process.Kill(true); } catch (InvalidOperationException) { }
                return new Status("error", "The upscaler scan did not finish.", null, false);
            }
            var line = stdout.Result.Trim().Split('\n').LastOrDefault() ?? "";
            using var json = JsonDocument.Parse(line);
            var root = json.RootElement;
            string Str(string name) => root.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
            var verified = root.TryGetProperty("runtime_verified", out var rv) && rv.ValueKind == JsonValueKind.True;
            return new Status(Str("state"), Str("line"), Str("profile") is { Length: > 0 } p ? p : null, verified);
        }
        catch (Exception e) when (e is IOException or JsonException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            return new Status("error", "Upscaler scan unavailable: " + e.Message, null, false);
        }
    }

    // One sentence for the per-game Upscaling page.
    public static string Describe(Game? game)
    {
        if (game is null)
            return "Automatic: when a game is added, KytyPS5 identifies its upscaler interface from the game files. Games with a " +
                   "supported interface get HLX upscaling and FSR 3 frame generation after Kyty has checked the live inputs in the " +
                   "first seconds of play; other games keep their own upscaler. PS / F10 opens the runtime upscaling menu.";
        var s = Get(game.Path);
        if (s is null) return File.Exists(Scanner) ? "Checking this game's upscaler interface..." : "Upscaler scanner not installed: " + Scanner;
        return s.Line;
    }
}

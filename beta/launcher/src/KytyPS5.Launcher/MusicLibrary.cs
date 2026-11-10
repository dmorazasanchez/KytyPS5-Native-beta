using System.Diagnostics;
using System.Security.Cryptography;

namespace KytyPS5;

internal sealed record MusicTheme(string Id, string Title, string Composition, string Arrangement, string Artist, string Source, string FileName);

internal static class MusicLibrary
{
    public static readonly MusicTheme[] Themes = [
        new("fairy", "Great Fairy's Fountain", "The Legend of Zelda: Ocarina of Time — Great Fairy's Fountain", "Garden of Venus", "RebeccaETripp",
            "https://ocremix.org/remix/OCR03924", "Legend_of_Zelda_Ocarina_of_Time_Garden_of_Venus_OC_ReMix.mp3"),
        new("dearly", "Dearly Beloved", "Kingdom Hearts — Dearly Beloved", "Dear Serenity", "Tune in with Chewie, Kristin Naigus, HOUNDTRACK",
            "https://ocremix.org/remix/OCR03886", "Kingdom_Hearts_2_Dear_Serenity_OC_ReMix.mp3"),
        new("enclosure", "Sniper Wolf's Theme (Enclosure)", "Metal Gear Solid — Sniper Wolf's Theme (Enclosure)", "Liquid Metal", "Vig",
            "https://ocremix.org/remix/OCR00895", "Metal_Gear_Solid_Liquid_Metal_OC_ReMix.mp3"),
        new("prelude", "Prelude", "Final Fantasy — Prelude", "Hymn to the Crystals", "RebeccaETripp",
            "https://ocremix.org/remix/OCR03621", "Final_Fantasy_4_Hymn_to_the_Crystals_OC_ReMix.mp3"),
        new("custom", "Custom music", "Custom music", "Local file", "Your library", "", "") ];
    public static string Root => Environment.GetEnvironmentVariable("KYTYPS5_AUDIO_ROOT") ?? Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local/share/KytyPS5/audio");
    private static readonly SemaphoreSlim Decoder = new(1, 1);
    public static string? Resolve(AudioPreferences options, string? themeId = null)
    {
        var id = themeId ?? options.Theme;
        if (options.Imports.TryGetValue(id, out var imported) && File.Exists(imported)) return imported;
        var theme = Themes.FirstOrDefault(t => t.Id == id);
        var path = theme is null || theme.FileName.Length == 0 ? null : Path.Combine(Root, "music", theme.FileName);
        return File.Exists(path) ? path : null;
    }

    public static async Task<string> PrepareAsync(string source, CancellationToken cancellation = default)
    {
        if (!File.Exists(source)) throw new FileNotFoundException("Music file not found.", source);
        await Decoder.WaitAsync(cancellation).ConfigureAwait(false);
        try
        {
            var info = new FileInfo(source);
            if (info.Length > 512L * 1024 * 1024) throw new InvalidDataException("Choose an audio file smaller than 512 MB.");
            using var input = File.OpenRead(source);
            var hash = Convert.ToHexString(await SHA256.HashDataAsync(input, cancellation).ConfigureAwait(false));
            var directory = Path.Combine(Root, "cache"); Directory.CreateDirectory(directory);
            var destination = Path.Combine(directory, hash + ".s16");
            if (File.Exists(destination) && new FileInfo(destination).Length >= 1920) return destination;
            var temporary = destination + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                var start = new ProcessStartInfo(Environment.GetEnvironmentVariable("KYTYPS5_FFMPEG") ?? "ffmpeg") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true,
                    ArgumentList = { "-nostdin", "-v", "error", "-y", "-i", source, "-map", "0:a:0", "-vn", "-t", "3600", "-af", "loudnorm=I=-20:TP=-2:LRA=11", "-ac", "2", "-ar", "48000", "-f", "s16le", temporary } };
                using var process = Process.Start(start) ?? throw new IOException("Could not start the audio decoder.");
                using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(3));
                using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellation, timeout.Token);
                using var registration = linked.Token.Register(() => { try { process.Kill(true); } catch (InvalidOperationException) { } });
                var errorTask = process.StandardError.ReadToEndAsync(linked.Token);
                await process.WaitForExitAsync(linked.Token).ConfigureAwait(false);
                var error = await errorTask.ConfigureAwait(false);
                if (process.ExitCode != 0 || !File.Exists(temporary) || new FileInfo(temporary).Length < 1920)
                    throw new InvalidDataException("Unsupported or damaged audio file. " + error[..Math.Min(error.Length, 220)]);
                File.Move(temporary, destination, true);
                return destination;
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }
        finally { Decoder.Release(); }
    }

    public static async Task<string> ImportAsync(string source, string id)
    {
        if (!Themes.Any(t => t.Id == id)) throw new ArgumentException("Unknown music theme.");
        // Validate completely before replacing the user's currently working track.
        await PrepareAsync(source).ConfigureAwait(false);
        var directory = Path.Combine(Root, "imports"); Directory.CreateDirectory(directory);
        using var file = File.OpenRead(source);
        var hash = Convert.ToHexString(await SHA256.HashDataAsync(file).ConfigureAwait(false))[..16];
        var extension = Path.GetExtension(source).ToLowerInvariant();
        var destination = Path.Combine(directory, id + "-" + hash + extension);
        if (Path.GetFullPath(source) != Path.GetFullPath(destination)) File.Copy(source, destination, true);
        return destination;
    }
}

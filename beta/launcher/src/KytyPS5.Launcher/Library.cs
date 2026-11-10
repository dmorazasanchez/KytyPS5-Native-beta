using System.Text.Json;
using System.Text.Json.Nodes;

namespace KytyPS5;

internal sealed class Preferences
{
    public static string Root => Environment.GetEnvironmentVariable("KYTYPS5_CONFIG_ROOT") ?? System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".config", "KytyPS5");
    public static string FilePath => System.IO.Path.Combine(Root, "launcher.json");
    public List<string> GameFolders { get; set; } = [];
    public List<string> ExcludedGames { get; set; } = [];
    public bool Fullscreen { get; set; } = Environment.GetEnvironmentVariable("XDG_CURRENT_DESKTOP") == "labwc";
    public string Theme { get; set; } = "Midnight";
    public bool PlayTitleMusic { get; set; }
    public int MusicVolume { get; set; } = 75;
    public string LastGame { get; set; } = "";
    public bool GridView { get; set; }
    public AudioPreferences Audio { get; set; } = new();
    [System.Text.Json.Serialization.JsonExtensionData]
    public Dictionary<string, JsonElement>? AdditionalProperties { get; set; }

    public static Preferences Load()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                var loaded = JsonSerializer.Deserialize<Preferences>(File.ReadAllText(FilePath)) ?? new();
                loaded.Audio ??= new(); loaded.Audio.Normalize(); return loaded;
            }
            var result = new Preferences();
            var legacy = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                ".local/share/kyty-sharp-frontend/gui-settings.json");
            if (File.Exists(legacy))
            {
                var json = JsonNode.Parse(File.ReadAllText(legacy));
                result.GameFolders = json?["GameFolders"]?.Deserialize<List<string>>() ?? [];
                result.ExcludedGames = json?["ExcludedGames"]?.Deserialize<List<string>>() ?? [];
                result.PlayTitleMusic = json?["PlayTitleMusic"]?.GetValue<bool>() ?? false;
                result.MusicVolume = (int)((json?["TitleMusicVolume"]?.GetValue<double>() ?? .75) * 100);
            }
            result.GameFolders = result.GameFolders.Concat(KytyBridge.ImportKytyGameFolders()).Distinct().ToList();
            return result;
        }
        catch (Exception e) when (e is IOException or JsonException or InvalidOperationException)
        {
            Console.Error.WriteLine("Launcher preferences: " + e.Message);
            return new();
        }
    }

    public void Save()
    {
        Directory.CreateDirectory(Root);
        File.WriteAllText(FilePath + ".new", JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        File.Move(FilePath + ".new", FilePath, true);
    }
}

internal sealed record Game(string Name, string TitleId, string Version, string Path, string? Icon, string? Background)
{
    public string Folder => GameArchive.MetadataFolder(Path);
    public static Game Read(string path)
    {
        if (Directory.Exists(path)) path = System.IO.Path.Combine(path, "eboot.bin");
        path = System.IO.Path.GetFullPath(path);
        var folder = GameArchive.MetadataFolder(path);
        var sys = System.IO.Path.Combine(folder, "sce_sys");
        var name = GameArchive.IsArchive(path) ? System.IO.Path.GetFileNameWithoutExtension(path) : System.IO.Path.GetFileName(folder);
        string id = "", version = "";
        try
        {
            var json = JsonNode.Parse(File.ReadAllText(System.IO.Path.Combine(sys, "param.json")));
            id = json?["titleId"]?.GetValue<string>() ?? "";
            version = json?["contentVersion"]?.GetValue<string>() ?? "";
            var localized = json?["localizedParameters"];
            name = localized?["en-US"]?["titleName"]?.GetValue<string>() ??
                localized?[json?["defaultLanguage"]?.GetValue<string>() ?? "en-US"]?["titleName"]?.GetValue<string>() ?? name;
        }
        catch (Exception e) when (e is IOException or JsonException or InvalidOperationException) { }
        string? Art(params string[] candidates) => candidates.Select(x => System.IO.Path.Combine(sys, x)).FirstOrDefault(File.Exists);
        return new Game(name, id, version, System.IO.Path.GetFullPath(path),
            Art("icon0.png", "icon0.dds", "icon0.jpg"), Art("pic1.png", "pic0.png", "pic1.jpg", "pic0.jpg"));
    }

    public static List<Game> Scan(Preferences settings)
    {
        var found = new Dictionary<string, Game>(StringComparer.Ordinal);
        var pending = new Queue<(string Path, int Depth)>();
        foreach (var root in settings.GameFolders.Distinct()) pending.Enqueue((System.IO.Path.GetFullPath(root), 0));
        var visited = new HashSet<string>();
        while (pending.Count > 0 && visited.Count < 10000)
        {
            var (path, depth) = pending.Dequeue();
            if (!visited.Add(path)) continue;
            try
            {
                var gameFolder = false;
                var eboot = File.Exists(path) ? path : System.IO.Path.Combine(path, "eboot.bin");
                if (File.Exists(eboot) && GameArchive.IsGameFile(eboot))
                {
                    if (!settings.ExcludedGames.Contains(eboot)) found[eboot] = Read(eboot);
                    if (File.Exists(path)) continue;
                    gameFolder = true;
                }
                if (!Directory.Exists(path)) continue;
                foreach (var archive in Directory.EnumerateFiles(path).Where(GameArchive.IsArchive))
                {
                    if (settings.ExcludedGames.Contains(archive) || found.ContainsKey(archive)) continue;
                    try { found[archive] = Read(archive); }
                    catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException) { }
                }
                if (gameFolder || depth >= 4) continue;
                foreach (var directory in Directory.EnumerateDirectories(path))
                    if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) == 0)
                        pending.Enqueue((directory, depth + 1));
            }
            catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException) { }
        }
        return found.Values.OrderBy(g => g.Name, StringComparer.OrdinalIgnoreCase).ToList();
    }
}

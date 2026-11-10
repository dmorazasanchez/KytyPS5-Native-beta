using System.Security.Cryptography;
using System.Text;
using SharpEmu.HLE.FileSystem;

namespace KytyPS5;

// Only library metadata is cached; the native runtime receives the original archive.
internal static class GameArchive
{
    private static readonly string[] MetadataFiles = [
        "param.json", "icon0.png", "icon0.dds", "icon0.jpg",
        "pic1.png", "pic0.png", "pic1.jpg", "pic0.jpg", "snd0.at9"
    ];

    public static bool IsArchive(string path) => path.EndsWith(".zar", StringComparison.OrdinalIgnoreCase);

    public static bool IsGameFile(string path) => IsArchive(path) ||
        System.IO.Path.GetFileName(path).Equals("eboot.bin", StringComparison.OrdinalIgnoreCase);

    public static string LaunchPath(string path)
    {
        if (!IsArchive(path)) return System.IO.Path.GetFullPath(path);
        var root = File.ReadAllText(System.IO.Path.Combine(MetadataFolder(path), ".game-root"));
        return System.IO.Path.GetFullPath(path) + (root.Length == 0 ? "" : "!/" + root);
    }

    private static string FindGameRoot(ZArchive archive)
    {
        var pending = new Queue<(string Path, int Depth)>(); pending.Enqueue(("", 0));
        string? found = null;
        var visited = 0;
        while (pending.Count > 0 && visited++ < 10000)
        {
            var (path, depth) = pending.Dequeue();
            var prefix = path.Length == 0 ? "" : path + "/";
            if (archive.IsFile(archive.LookUp(prefix + "eboot.bin")))
            {
                if (path.Length == 0) return "";
                if (found is not null) throw new InvalidDataException("Archive contains multiple game folders: " + archive.Path);
                found = path; continue;
            }
            if (depth >= 8) continue;
            foreach (var entry in archive.GetEntries(archive.LookUp(path)))
                if (entry.IsDirectory) pending.Enqueue((prefix + entry.Name, depth + 1));
        }
        return found ?? throw new InvalidDataException("Game archive has no eboot.bin: " + archive.Path);
    }

    public static string MetadataFolder(string path)
    {
        if (!IsArchive(path)) return Directory.Exists(path) ? path : System.IO.Path.GetDirectoryName(path)!;
        path = System.IO.Path.GetFullPath(path);
        var info = new FileInfo(path);
        var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(path))).ToLowerInvariant();
        var folder = System.IO.Path.Combine(Preferences.Root, "archive-cache", key);
        var stampPath = System.IO.Path.Combine(folder, ".archive-stamp");
        var stamp = $"2|{info.Length}|{info.LastWriteTimeUtc.Ticks}";
        if (File.Exists(stampPath) && File.ReadAllText(stampPath) == stamp) return folder;

        using var archive = ZArchive.Open(path) ?? throw new InvalidDataException("Unreadable game archive: " + path);
        var gameRoot = FindGameRoot(archive);
        if (Directory.Exists(folder)) Directory.Delete(folder, true);
        Directory.CreateDirectory(System.IO.Path.Combine(folder, "sce_sys"));
        foreach (var name in MetadataFiles)
        {
            var node = archive.LookUp((gameRoot.Length == 0 ? "" : gameRoot + "/") + "sce_sys/" + name);
            if (!archive.IsFile(node)) continue;
            var size = archive.GetFileSize(node);
            if (size > 32 * 1024 * 1024) continue;
            var data = new byte[(int)size];
            try
            {
                if (archive.Read(node, 0, data) != data.Length)
                    throw new InvalidDataException("Incomplete archive metadata: " + name);
            }
            catch (ZstdSharp.ZstdException error)
            {
                throw new InvalidDataException("Damaged archive metadata: " + name, error);
            }
            File.WriteAllBytes(System.IO.Path.Combine(folder, "sce_sys", name), data);
        }
        File.WriteAllText(System.IO.Path.Combine(folder, ".game-root"), gameRoot);
        File.WriteAllText(stampPath, stamp);
        return folder;
    }
}

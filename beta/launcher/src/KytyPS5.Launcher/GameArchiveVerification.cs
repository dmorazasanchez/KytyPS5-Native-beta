using System.Buffers.Binary;
using System.Text;
using ZstdSharp;

namespace KytyPS5;

internal static class GameArchiveVerification
{
    public static void Run(string root, Action<bool, string> check)
    {
        var library = Path.Combine(root, "archives"); Directory.CreateDirectory(library);
        var archive = Path.Combine(library, "Archive with spaces & symbols.ZAR");
        WriteArchive(archive, "Archive Game", "01.000.000");
        var game = Game.Read(archive);
        check(game.Name == "Archive Game" && game.TitleId == "PPSATEST1" && game.Version == "01.000.000",
            "Compressed archive metadata and uppercase extension load");
        check(game.Path == archive && File.Exists(game.Icon) && !File.Exists(Path.Combine(game.Folder, "eboot.bin")),
            "Archive artwork is cached without extracting game content");
        File.WriteAllText(Path.Combine(library, "broken.zar"), "not an archive");
        File.WriteAllText(Path.Combine(library, "unrelated.txt"), "not a game");
        var extracted = Path.Combine(library, "extracted"); Directory.CreateDirectory(extracted);
        File.WriteAllText(Path.Combine(extracted, "eboot.bin"), "fixture");
        var settings = new Preferences { GameFolders = [library, archive, Path.Combine(library, ".")] };
        check(Game.Scan(settings).Count == 2, "Folder scans include archives and extracted games, deduplicate paths and skip broken archives");
        settings.ExcludedGames.Add(archive);
        check(Game.Scan(settings).All(g => g.Path != archive), "Excluded archives stay hidden for folder and direct-file roots");
        settings = new Preferences { GameFolders = [archive] };
        check(Game.Scan(settings).Single().Path == archive, "An archive can be added directly");
        var command = KytyBridge.Build(archive, game.TitleId);
        check(command.Arguments[0] == "--game" && command.Arguments[1] == archive && command.Arguments.Count(a => a == archive) == 1,
            "The original archive path reaches the native runtime as one argument");
        var patch = Path.Combine(library, "patch.json");
        File.WriteAllText(patch, """{"id":"PPSATEST1","version":"01.000.000","mods":[]}""");
        check(KytyBridge.PatchMatchesGame(patch, archive, game.TitleId), "Archive cheats use the archive's title and exact version");
        WriteArchive(archive, "Updated Archive Game", "02.000.000", withIcon: false);
        var updated = Game.Read(archive);
        check(updated.Name == "Updated Archive Game" && updated.Version == "02.000.000" && updated.Icon is null &&
            !KytyBridge.PatchMatchesGame(patch, archive, game.TitleId), "Replacing an archive refreshes metadata, removes stale art and rejects old cheats");
        var other = Path.Combine(library, "other"); Directory.CreateDirectory(other);
        var otherArchive = Path.Combine(other, Path.GetFileName(archive)); WriteArchive(otherArchive, "Other Game", "03.000.000");
        check(Game.Read(otherArchive).Folder != updated.Folder && Game.Read(archive).Name == updated.Name,
            "Archives with the same filename have separate metadata caches");
        var nongame = Path.Combine(library, "no-eboot.zar"); WriteArchive(nongame, "Not a game", "01.000.000", withEboot: false);
        check(Game.Scan(new Preferences { GameFolders = [nongame] }).Count == 0, "Archives without an executable are skipped");
        var deep = Path.Combine(library, "a", "b", "c", "d"); Directory.CreateDirectory(deep);
        var deepArchive = Path.Combine(deep, "deep.zar"); WriteArchive(deepArchive, "Deep Game", "01.000.000");
        check(Game.Scan(new Preferences { GameFolders = [library] }).Any(g => g.Path == deepArchive),
            "Archives at the library depth boundary are discovered");
        var nestedArchive = Path.Combine(extracted, "nested.zar");
        WriteArchive(nestedArchive, "Nested Game", "04.000.000", nestedRoot: true);
        var nestedGame = Game.Read(nestedArchive);
        check(nestedGame.Name == "Nested Game" && KytyBridge.Build(nestedArchive, nestedGame.TitleId).Arguments[1] == nestedArchive + "!/Game folder",
            "Archives containing a game subfolder pass the native virtual directory without extraction");
        check(Game.Scan(new Preferences { GameFolders = [extracted] }).Count == 2,
            "Archives beside an extracted executable remain discoverable");
    }

    // A small, independently laid out version-1 ZArchive with one compressed 64 KiB block.
    private static void WriteArchive(string path, string title, string version, bool withIcon = true, bool withEboot = true, bool nestedRoot = false)
    {
        var param = Encoding.UTF8.GetBytes(System.Text.Json.JsonSerializer.Serialize(new {
            titleId = "PPSATEST1", contentVersion = version,
            localizedParameters = new Dictionary<string, object> { ["en-US"] = new { titleName = title } }
        }));
        byte[] icon = [1, 2, 3, 4];
        var block = new byte[65536];
        param.CopyTo(block, 8); icon.CopyTo(block, 8 + param.Length);
        using var compressor = new Compressor();
        var compressed = compressor.Wrap(block).ToArray();
        using var output = new FileStream(path, FileMode.Create, FileAccess.Write);
        void U32(uint value) { Span<byte> bytes = stackalloc byte[4]; BinaryPrimitives.WriteUInt32BigEndian(bytes, value); output.Write(bytes); }
        void U64(ulong value) { Span<byte> bytes = stackalloc byte[8]; BinaryPrimitives.WriteUInt64BigEndian(bytes, value); output.Write(bytes); }
        void Node(uint name, uint first, uint size) { U32(name); U32(first); U32(size); U32(0); }
        output.Write(compressed);
        var recordsOffset = (ulong)output.Position;
        U64(0); Span<byte> sizes = stackalloc byte[32]; sizes.Clear();
        BinaryPrimitives.WriteUInt16BigEndian(sizes, (ushort)(compressed.Length - 1)); output.Write(sizes);
        var namesOffset = (ulong)output.Position;
        var names = new MemoryStream();
        uint Name(string name) { var offset = (uint)names.Position; var bytes = Encoding.UTF8.GetBytes(name); names.WriteByte((byte)bytes.Length); names.Write(bytes); return offset; }
        var rootName = Name(""); var ebootName = Name("eboot.bin"); var sysName = Name("sce_sys"); var paramName = Name("param.json"); var iconName = Name("icon0.png");
        var folderName = Name("Game folder");
        output.Write(names.ToArray());
        var treeOffset = (ulong)output.Position;
        Node(rootName, 1, nestedRoot ? 1u : withEboot ? 2u : 1u);
        if (nestedRoot) Node(folderName, 2, withEboot ? 2u : 1u);
        if (withEboot) Node(ebootName | 0x80000000, 0, 8);
        Node(sysName, (withEboot ? 3u : 2u) + (nestedRoot ? 1u : 0u), withIcon ? 2u : 1u);
        Node(paramName | 0x80000000, 8, (uint)param.Length);
        if (withIcon) Node(iconName | 0x80000000, (uint)(8 + param.Length), (uint)icon.Length);
        var treeSize = (ulong)output.Position - treeOffset;
        var total = (ulong)output.Position + 144;
        U64(0); U64((ulong)compressed.Length); U64(recordsOffset); U64(40);
        U64(namesOffset); U64((ulong)names.Length); U64(treeOffset); U64(treeSize);
        output.Write(new byte[64]); U64(total); U32(0x61BF3A01); U32(0x169F52D6);
    }
}

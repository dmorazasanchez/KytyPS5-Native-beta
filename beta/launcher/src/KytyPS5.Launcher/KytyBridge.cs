// Copyright (C) 2026 SharpEmu Emulator Project; BC-250 frontend adaptation.
// SPDX-License-Identifier: GPL-2.0-or-later
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace KytyPS5;

/// <summary>
/// SharpEmu visual frontend -> independently built Kyty native BC-250 emulator.
/// Never mutates SharpEmu's core, the existing Kyty Qt INI, or game files.
/// </summary>
internal static class KytyBridge
{
    internal record Option(string Id, string Label, string Group, string Default, string[]? Choices = null, string? Flag = null, bool Boolean = false)
    {
        public bool IsEnvironment => Id.StartsWith("KYTY_", StringComparison.Ordinal) || Id.StartsWith("ENABLE_LSFGVK", StringComparison.Ordinal);
    }

    public static bool Enabled => true;
    public static string EmulatorPath => Environment.GetEnvironmentVariable("KYTY_SHARP_EMULATOR")
        ?? Path.Combine(AppContext.BaseDirectory, "../emulator/kyty_emulator_native");
    private static string DirectoryRoot => Environment.GetEnvironmentVariable("KYTYPS5_PROFILE_ROOT") ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".config", "kyty-sharp-frontend");
    public static IReadOnlyList<Option> Options { get; } = new List<Option>
    {
        new("resolution", "Rendering resolution", "Display", "2560x1440", ["1920x1080","2560x1440","3840x2160","1280x720"]),
        new("fullscreen", "Borderless fullscreen", "Display", "1", ["1","0"], Boolean:true),
        new("hdr", "HDR10 when available", "Display", "0", ["0","1"], Boolean:true),
        new("present", "Vulkan present mode", "Display", "Fifo", ["Fifo","Mailbox","Immediate"], "--present-mode"),
        new("vblank", "PS5 virtual refresh (Hz)", "Display", "60", ["30","60","120"], "--vblank-frequency"),
        new("gpu", "Host GPU index", "Display", "0", ["0","1","2"], "--gpu"),
        new("overlay", "Performance overlay (F2)", "Display", "1", ["1","0"], Boolean:true),
        new("nosplash", "Skip splash and notices", "Display", "1", ["1","0"], Boolean:true),
        new("shaderopt", "Shader optimization", "Display", "Performance", ["Performance","Size","None"], "--shader-optimization-type"),
        new("readbackLinear", "Readback linear images", "Display", "0", ["0","1"], Boolean:true),
        new("readbackTiled", "Readback tiled images", "Display", "0", ["0","1"], Boolean:true),
        new("rawsync", "Synchronize raw image buffers", "Display", "0", ["0","1"], Boolean:true),
        new("tessellation", "Enable tessellation", "Display", "0", ["0","1"], Boolean:true),

        new("KYTY_NATIVE_GPU_PM4_MODE", "GPU command translation", "BC-250 Renderer", "upstream", ["fast","upstream"]),
        new("KYTY_NATIVE_GPU_UNRESTRICTED_MEMORY", "GPU memory budget (1=unrestricted)", "BC-250 Renderer", "1", ["1","0"]),
        new("KYTY_NATIVE_GPU_RESIDENCY", "GPU memory residency", "BC-250 Renderer", "partial", ["partial","full"]),
        new("KYTY_NATIVE_GPU_LABEL_WB", "GPU label writebacks (-1=mode default)", "BC-250 Renderer", "-1", ["-1","0","1","2"]),
        new("KYTY_NATIVE_GPU_TABLE_PREFETCH", "PM4 table prefetch (-1=auto)", "BC-250 Renderer", "-1", ["-1","0","1"]),
        new("KYTY_NATIVE_GPU_TABLE_PROMOTE", "Descriptor table promotion (-1=auto)", "BC-250 Renderer", "-1", ["-1","0","1"]),
        new("KYTY_NATIVE_GPU_TABLE_SCAN_DWORDS", "Descriptor scan DWORDs (0=default)", "BC-250 Renderer", "0", ["0","64","96","128"]),
        new("KYTY_NATIVE_GPU_BLOCK_MIB", "GTT block size (MiB)", "BC-250 Renderer", "16", ["16","32","64","128"]),
        new("KYTY_NATIVE_GPU_COHERENT_TYPES", "Coherent memory types", "BC-250 Renderer", "none", ["none","all"]),
        new("KYTY_NATIVE_GPU_GEM", "Native GEM backing", "BC-250 Renderer", "1", ["1","0"]),
        new("KYTY_NATIVE_GPU_PRIMARY", "Native GPU primary renderer", "BC-250 Renderer", "1", ["1","0"]),
        new("KYTY_NATIVE_GPU", "Native GPU override (-1=auto)", "BC-250 Renderer", "-1", ["-1","0","1"]),

        new("KYTY_NATIVE_GPU_KFD_QUEUES", "KFD hardware queues", "GPU / Synchronization", "0", ["0","1"]),
        new("KYTY_NATIVE_GPU_KFD_MEMORY", "KFD memory path", "GPU / Synchronization", "0", ["0","1"]),
        new("KYTY_NATIVE_GPU_KFD_IB", "KFD indirect buffers", "GPU / Synchronization", "0", ["0","1"]),
        new("KYTY_NATIVE_GPU_ACB_RINGS", "ACB rings", "GPU / Synchronization", "0", ["0","1"]),
        new("KYTY_NATIVE_GPU_ACB_DEFER", "Defer ACB", "GPU / Synchronization", "-1", ["-1","0","1"]),
        new("KYTY_NATIVE_GPU_IRQ", "IRQ mode", "GPU / Synchronization", "0", ["0","1"]),
        new("KYTY_NATIVE_GPU_STOCK_IRQ", "Use stock IRQ", "GPU / Synchronization", "0", ["0","1"]),
        new("KYTY_NATIVE_GPU_FLIP_STATS", "Flip diagnostics", "GPU / Synchronization", "1", ["0","1"]),
        new("KYTY_LOD_STATS_MODE", "LOD stats mode", "GPU / Synchronization", "auto", ["auto","banks"]),
        new("KYTY_NATIVE_GPU_SKIP_PS", "Skip Demon's Souls HDR pass (version-specific)", "GPU / Synchronization", "off", ["off","0x208a7ca00","0x208a8e100"]),
        new("KYTY_DEMONS_SCREAM_ASSERT_RECOVERY", "Demon's Souls experimental assert recovery", "GPU / Synchronization", "0", ["0","1"]),
        new("fg_mode", "Lossless Scaling FG (Off / 2x / Adaptive 60 FPS)", "Frame Generation", "off", ["off","fixed","adaptive"]),
        // Native upscaler build: titles whose upscaler interface kyty-upscaler-scan identified (UpscalerProfiles);
        // the runtime selector checks the game's profile, other titles ignore these and keep their own upscaler.
        // "auto": HLX FSR 4 once Kyty has verified the live inputs; frame generation when the title runs below 50 FPS.
        new("upscaler", "Upscaler", "Upscaling", "auto", ["auto","game","hlx_fsr4","hlx_dlss_e"]),
        new("upscale_output", "Upscaled output resolution", "Upscaling", "display", ["display","3840x2160","3200x1800","2560x1440","1920x1080"]),
        new("upscale_preset", "Render resolution preset", "Upscaling", "dynamic",
            ["dynamic","dlaa","ultra_quality","quality","balanced","performance","ultra_performance"]),
        // Kyty's own frame generation (FSR 3.1 interpolation in the presenter, from the game's depth and motion vectors).
        new("framegen", "Native frame generation", "Upscaling", "auto", ["auto","off","fsr3"]),

        new("KYTY_BC250_CPU_AFFINITY", "Host CPU affinity", "CPU / Controller", "auto", ["auto","0-3","4-7","0-7"]),
        new("KYTY_BC250_CPU_NICE", "Host CPU nice", "CPU / Controller", "0", ["0","5","10"]),
        new("KYTY_BC250_AUDIO_TEST", "Demon's Souls audio mix (Original / Dialogue test)", "CPU / Controller", "off", ["off","dialogue"]),
        new("language", "Console language", "CPU / Controller", "1", ["0","1","2","3","4","5"], "--console-language"),
        new("user", "PS5 user name", "CPU / Controller", "Kyty", null, "--user-name"),
        new("userId", "PS5 user ID", "CPU / Controller", "1000", ["1000","1001","1002"], "--user-id"),
        new("volume", "DualSense speaker volume", "CPU / Controller", "50", ["0","25","50","75","100"], "--controller-volume"),
        new("vibration", "DualSense vibration intensity", "CPU / Controller", "100", ["0","25","50","75","100"], "--controller-vibration"),
        new("keyboard", "Keyboard player", "CPU / Controller", "0", ["0","1","2","3","4"], "--keyboard-player"),
        new("mic", "Capture microphone name (optional)", "CPU / Controller", "", null, "--mic"),
        new("lightbar", "DualSense lightbar color (#RRGGBB)", "CPU / Controller", "", null, "--controller-color"),
        new("keymap", "Custom controller mapping (one Control=Input per line)", "CPU / Controller", ""),
        new("amd", "AMD CPU patches", "CPU / Controller", "0", ["0","1"], Boolean:true),

        new("patch", "ETAHen patches JSON path (optional)", "Patches / Advanced", ""),
        new("addcont", "DLC directory (optional)", "Patches / Advanced", ""),
        new("shaderlog", "Shader log output", "Patches / Advanced", "Silent", ["Silent","Console","File"], "--shader-log-direction"),
        new("printf", "Guest printf output", "Patches / Advanced", "Silent", ["Silent","Console","File"], "--printf-direction"),
        new("validation", "Vulkan validation (slow)", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("shaderValidate", "Shader validation", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("gpuAssist", "GPU-assisted validation (very slow)", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("commandDump", "GPU command-buffer dumps", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("debugDump", "Graphics debug dumps", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("profile", "Tracy CPU/GPU profiling", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("spirvPrint", "SPIR-V debug printf", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("renderdoc", "RenderDoc capture", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("network", "Simulated network connected", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("playgo", "PlayGo compatibility fallback", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("vr", "Virtual-reality headset emulation", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("hideCursor", "Hide cursor after inactivity", "Display", "1", ["0","1"], Boolean:true),
        new("noTrophy", "Disable trophy notifications", "Patches / Advanced", "0", ["0","1"], Boolean:true),
        new("trophySeconds", "Trophy notification duration (5–10 s)", "Patches / Advanced", "6", ["5","6","7","8","9","10"], "--trophy-duration"),
        new("shaderFolder", "Shader dump folder", "Patches / Advanced", "", null, "--shader-log-folder"),
        new("bufferFolder", "Command-buffer dump folder", "Patches / Advanced", "", null, "--command-buffer-dump-folder"),
        new("printfFolder", "Guest printf output file", "Patches / Advanced", "", null, "--printf-output-file"),
    };

    public static List<string> ImportKytyGameFolders()
    {
        var found = new HashSet<string>(StringComparer.Ordinal);
        var ini = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            ".config", "Kyty", "Kyty.ini");
        if (!File.Exists(ini)) return [];
        var inGames = false;
        foreach (var line in File.ReadLines(ini))
        {
            if (line.StartsWith('[')) { inGames = line.Trim() == "[GameConfigurations]"; continue; }
            if (!inGames) continue;
            var equal = line.IndexOf('=');
            if (equal < 0 || !line[..equal].EndsWith("\\game_path", StringComparison.Ordinal)) continue;
            var directory = line[(equal + 1)..].Trim();
            if ((Directory.Exists(directory) && File.Exists(Path.Combine(directory, "eboot.bin"))) ||
                (GameArchive.IsArchive(directory) && File.Exists(directory)))
                found.Add(directory);
        }
        return found.OrderBy(x => x, StringComparer.Ordinal).ToList();
    }

    public static string ProfileFile(string? titleId, string? ebootPath)
    {
        if (string.IsNullOrWhiteSpace(ebootPath))
            return Path.Combine(DirectoryRoot, "global.json");
        var bytes = SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(ebootPath)));
        var tag = Convert.ToHexString(bytes)[..12].ToLowerInvariant();
        var title = string.IsNullOrWhiteSpace(titleId) ? "game" : new string(titleId.Where(char.IsLetterOrDigit).ToArray());
        return Path.Combine(DirectoryRoot, $"{title}-{tag}.json");
    }

    public static Dictionary<string, string> Load(string? titleId, string? gamePath)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var option in Options) result[option.Id] = option.Default;
        ImportExistingKytyPreferences(result, gamePath);
        foreach (var file in new[] { ProfileFile(null, null), ProfileFile(titleId, gamePath) }.Distinct())
        {
            if (!File.Exists(file)) continue;
            try
            {
                foreach (var (key, value) in JsonSerializer.Deserialize<Dictionary<string,string>>(File.ReadAllText(file)) ?? [])
                    result[key] = value;
            }
            catch (Exception error) { Console.Error.WriteLine($"[KytyBridge] Invalid options {file}: {error.Message}"); }
        }
        // Beta ships opt-in, exact-version cheat plans in a writable per-user directory.
        var bundledPatches = Environment.GetEnvironmentVariable("KYTYPS5_PATCH_ROOT");
        if (string.IsNullOrEmpty(result.GetValueOrDefault("patch")) && gamePath is not null &&
            bundledPatches is not null && Directory.Exists(bundledPatches))
        {
            var match = Directory.EnumerateFiles(bundledPatches, "*.json")
                .FirstOrDefault(p => PatchMatchesGame(p, gamePath, titleId));
            if (match is not null) result["patch"] = match;
        }
        return result;
    }

    // Read-only compatibility import. The user's Qt profiles and saves are NEVER rewritten.
    private static void ImportExistingKytyPreferences(Dictionary<string, string> output, string? gamePath)
    {
        var file = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            ".config", "Kyty", "Kyty.ini");
        if (!File.Exists(file)) return;
        var global = new Dictionary<string, string>();
        var games = new Dictionary<string, Dictionary<string, string>>();
        string section = "";
        foreach (var line in File.ReadLines(file))
        {
            var trimmed = line.Trim();
            if (trimmed.StartsWith('[') && trimmed.EndsWith(']'))
            {
                section = trimmed[1..^1];
                continue;
            }
            var equals = line.IndexOf('=');
            if (equals <= 0) continue;
            var key = line[..equals];
            var value = line[(equals + 1)..];
            if (section == "GlobalConfiguration") global[key] = value;
            if (section != "GameConfigurations") continue;
            var slash = key.IndexOf('\\');
            if (slash < 0) continue;
            var id = key[..slash];
            if (!games.TryGetValue(id, out var entries))
                games[id] = entries = new Dictionary<string, string>();
            entries[key[(slash + 1)..]] = value;
        }
        void Apply(Dictionary<string, string> info)
        {
            string V(string name) => info.GetValueOrDefault(name) ?? "";
            void Set(string name, string key)
            {
                if (info.ContainsKey(name) && output.ContainsKey(key)) output[key] = V(name);
            }
            void Bool(string name, string key)
            {
                if (info.ContainsKey(name)) output[key] = V(name).Equals("true", StringComparison.OrdinalIgnoreCase) ? "1" : "0";
            }
            Set("screen_resolution", "resolution");
            Bool("fullscreen_enabled", "fullscreen");
            Bool("hdr_enabled", "hdr");
            Set("present_mode", "present");
            Set("vblank_frequency", "vblank");
            Set("gpu_index", "gpu");
            Bool("perf_overlay_enabled", "overlay");
            Bool("skip_notice_screen", "nosplash");
            Set("shader_optimization_type", "shaderopt");
            Bool("readback_linear_images", "readbackLinear");
            Bool("readback_tiled_images", "readbackTiled");
            Bool("sync_raw_image_buffers", "rawsync");
            Bool("tessellation_enabled", "tessellation");
            Bool("amd_cpu_enabled", "amd");
            Set("console_language", "language");
            Set("user_name", "user");
            Set("controller_speaker_volume", "volume");
            Set("controller_vibration_intensity", "vibration");
            Set("controller_keyboard_player", "keyboard");
            Set("shader_log_direction", "shaderlog");
            Set("printf_direction", "printf");
            Bool("vulkan_validation_enabled", "validation");
            Set("bc250_cpu_affinity", "KYTY_BC250_CPU_AFFINITY");
            Set("bc250_cpu_nice", "KYTY_BC250_CPU_NICE");
            Set("bc250_residency", "KYTY_NATIVE_GPU_RESIDENCY");
            Set("bc250_label_wb", "KYTY_NATIVE_GPU_LABEL_WB");
            Set("bc250_table_prefetch", "KYTY_NATIVE_GPU_TABLE_PREFETCH");
            Set("bc250_table_promote", "KYTY_NATIVE_GPU_TABLE_PROMOTE");
            Set("bc250_table_scan_dwords", "KYTY_NATIVE_GPU_TABLE_SCAN_DWORDS");
            Set("bc250_block_mib", "KYTY_NATIVE_GPU_BLOCK_MIB");
            Set("bc250_coherent", "KYTY_NATIVE_GPU_COHERENT_TYPES");
            Set("bc250_native_gpu", "KYTY_NATIVE_GPU");
            Set("bc250_kfd_queues", "KYTY_NATIVE_GPU_KFD_QUEUES");
            Set("bc250_acb_rings", "KYTY_NATIVE_GPU_ACB_RINGS");
            Set("bc250_acb_defer", "KYTY_NATIVE_GPU_ACB_DEFER");
            Set("bc250_irq", "KYTY_NATIVE_GPU_IRQ");
            Set("bc250_flip_stats", "KYTY_NATIVE_GPU_FLIP_STATS");
            Set("bc250_lod_stats_mode", "KYTY_LOD_STATS_MODE");
            if (V("bc250_pm4_translation") is "fast" or "upstream-pm4")
                output["KYTY_NATIVE_GPU_PM4_MODE"] = V("bc250_pm4_translation") == "fast" ? "fast" : "upstream";
            if (V("bc250_memory_budget") is "normal" or "unrestricted")
                output["KYTY_NATIVE_GPU_UNRESTRICTED_MEMORY"] = V("bc250_memory_budget") == "normal" ? "0" : "1";
        }
        Apply(global);
        if (gamePath is null) return;
        var folder = Directory.Exists(gamePath) || GameArchive.IsArchive(gamePath) ? gamePath : Path.GetDirectoryName(gamePath);
        if (folder is null) return;
        foreach (var entries in games.Values)
        {
            if (!entries.GetValueOrDefault("game_path", "").Equals(folder, StringComparison.Ordinal)) continue;
            if (entries.GetValueOrDefault("custom_settings", "false") == "true") Apply(entries);
            break;
        }
    }

    public static void Save(string? titleId, string? gamePath, IReadOnlyDictionary<string, string> values)
    {
        var file = ProfileFile(titleId, gamePath);
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        var temp = file + ".new";
        File.WriteAllText(temp, JsonSerializer.Serialize(values, new JsonSerializerOptions { WriteIndented = true }));
        File.Move(temp, file, overwrite: true);
    }

    public static bool PatchMatchesGame(string patchPath, string gamePath, string? titleId)
    {
        try
        {
            var folder = GameArchive.MetadataFolder(gamePath);
            if (folder is null) return false;
            var paramPath = Path.Combine(folder, "sce_sys", "param.json");
            if (!File.Exists(paramPath)) return false;
            using var patch = JsonDocument.Parse(File.ReadAllText(patchPath));
            using var param = JsonDocument.Parse(File.ReadAllText(paramPath));
            var actualId = param.RootElement.GetProperty("titleId").GetString();
            var actualVersion = param.RootElement.GetProperty("contentVersion").GetString();
            var patchId = patch.RootElement.GetProperty("id").GetString();
            var patchVersion = patch.RootElement.GetProperty("version").GetString();
            return !string.IsNullOrEmpty(titleId) && titleId == actualId &&
                titleId == patchId && actualVersion == patchVersion;
        }
        catch (Exception) { return false; }
    }

    public sealed record Command(string Exe, List<string> Arguments, IReadOnlyDictionary<string, string?> Environment);

    public static Command Build(string ebootPath, string? titleId)
    {
        var values = Load(titleId, ebootPath);
        string Value(string id) => values.GetValueOrDefault(id) ?? "";
        // An upstream launch must not inherit the frontend\'s BC-250 experiment defaults.
        var cleanUpstream = Value("KYTY_NATIVE_GPU_PM4_MODE") == "upstream";
        var args = new List<string> { "--game", GameArchive.LaunchPath(ebootPath) };
        var env = new Dictionary<string, string?>(StringComparer.Ordinal)
        {
            ["KYTY_NATIVE_GPU_GEM"] = "1",
            ["KYTY_NATIVE_GPU_PRIMARY"] = "1",
            ["KYTY_NATIVE_GPU_KFD_QUEUES"] = "0",
            ["KYTY_NATIVE_GPU_KFD_MEMORY"] = "0",
            ["KYTY_NATIVE_GPU_KFD_IB"] = "0",
            ["KYTY_NATIVE_GPU_STOCK_IRQ"] = "0",
            ["KYTY_NATIVE_GPU_ACB_RINGS"] = "0",
            ["KYTY_NATIVE_GPU_RESIDENCY"] = "partial",
            ["KYTY_NATIVE_GPU_BLOCK_MIB"] = "16",
            ["KYTY_NATIVE_GPU_COHERENT_TYPES"] = "none",
            ["KYTY_NATIVE_GPU_IRQ"] = "0",
            ["KYTY_NATIVE_GPU_FLIP_STATS"] = "1",
            ["KYTY_LOD_HW_SOCK"] = "/run/kyty-lodhw.sock",
            ["KYTY_NATIVE_GPU_PM4_MODE"] = "fast",
            ["KYTY_NATIVE_GPU_UNRESTRICTED_MEMORY"] = "1",
        };
        foreach (var option in Options)
        {
            var value = Value(option.Id);
            if (option.IsEnvironment)
            {
                // Imported Qt profiles use "auto" and "0" as sentinels. Do not erase
                // BC-250 safety defaults established above: notably RESIDENCY=partial.
                // Removing that value switches GemBacking to eager map-time residency,
                // which can make Demon's Souls 1.007 crash before the renderer starts.
                if (value is "" or "auto" or "-1" or "off" ||
                    (option.Id == "KYTY_NATIVE_GPU_TABLE_SCAN_DWORDS" && value == "0") ||
                    (option.Id == "KYTY_NATIVE_GPU_BLOCK_MIB" && value == "0"))
                    continue;
                env[option.Id] = value;
                continue;
            }
            if (option.Boolean || option.Flag is null) continue;
            if (value.Length > 0) { args.Add(option.Flag); args.Add(value); }
        }

        var resolution = Value("resolution").Split('x', 2);
        if (resolution.Length == 2 &&
            int.TryParse(resolution[0], out var width) && width is >= 640 and <= 7680 &&
            int.TryParse(resolution[1], out var height) && height is >= 480 and <= 4320)
        {
            args.AddRange(["--screen-width", width.ToString(), "--screen-height", height.ToString()]);
        }
        if (Value("fullscreen") == "1") args.Add("--fullscreen");
        if (Value("hdr") == "1") args.Add("--hdr");
        if (Value("overlay") == "1") args.Add("--perf-overlay");
        if (Value("nosplash") == "1") args.AddRange(["--no-splash", "--skip-notice-screen"]);
        if (Value("amd") == "1") args.Add("--amd-cpu");
        if (Value("readbackLinear") == "1") args.Add("--readback-linear-images");
        if (Value("readbackTiled") == "1") args.Add("--readback-tiled-images");
        if (Value("rawsync") == "1") args.Add("--sync-raw-image-buffers");
        if (Value("tessellation") == "1") args.Add("--tessellation");
        if (Value("validation") == "1") args.Add("--vulkan-validation");
        foreach (var (id, flag) in new (string, string)[] {
            ("shaderValidate", "--shader-validation"),
            ("gpuAssist", "--gpu-assisted-validation"),
            ("commandDump", "--command-buffer-dump"),
            ("debugDump", "--graphics-debug-dump"),
            ("profile", "--profile"),
            ("spirvPrint", "--spirv-debug-printf"),
            ("renderdoc", "--rd"),
            ("network", "--network-connected"),
            ("playgo", "--playgo-hack"),
            ("vr", "--vr"),
            ("hideCursor", "--hide-cursor"),
            ("noTrophy", "--no-trophy-notifications") })
            if (Value(id) == "1") args.Add(flag);
        foreach (var mapping in Value("keymap").Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            if (!mapping.Contains('=') || mapping.Length > 160) continue;
            args.AddRange(["--keymap", mapping]);
        }
        if (File.Exists(Value("patch")) && PatchMatchesGame(Value("patch"), ebootPath, titleId))
            args.AddRange(["--game-patch", Value("patch")]);
        if (!cleanUpstream && Directory.Exists(Value("addcont"))) args.AddRange(["--addcont-dir", Value("addcont")]);

        if (!string.IsNullOrWhiteSpace(titleId)) env["KYTY_BC250_TITLE_ID"] = titleId;
        if (Value("KYTY_LOD_STATS_MODE") == "banks") env["KYTY_NATIVE_GPU_LOD_STATS"] = "0";
        // Pass only a narrow presentation-layer choice to the executable selector.
        // Upstream's native GPU flags remain untouched; the selector injects LSFG
        // after filtering all BC-250 experimental renderer overrides.
        var fgMode = Value("fg_mode");
        if (fgMode is not ("off" or "fixed" or "adaptive")) fgMode = "off";
        if (titleId != "PPSA01341")
        {
            env.Remove("KYTY_NATIVE_GPU_SKIP_PS");
            env.Remove("KYTY_DEMONS_SCREAM_ASSERT_RECOVERY");
        }
        // A line = one argument, preserving paths with whitespace without shell evaluation.
        if (!cleanUpstream && values.TryGetValue("extraArguments", out var extra))
            args.AddRange(extra.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
        if (!cleanUpstream && values.TryGetValue("extraEnvironment", out var extraEnv))
        {
            foreach (var line in extraEnv.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            {
                var equals = line.IndexOf('=');
                if (equals < 1) continue;
                var key = line[..equals].Trim();
                if (key.All(ch => char.IsAsciiLetterOrDigit(ch) || ch == '_') &&
                    (char.IsAsciiLetter(key[0]) || key[0] == '_'))
                    env[key] = line[(equals + 1)..];
            }
        }

        var root = GameArchive.MetadataFolder(ebootPath);
        var paramFile = root is null ? "" : Path.Combine(root, "sce_sys", "param.json");
        // Pixel-shader addresses are tied to the *exact* installed game build.
        // 1.004's 320x180 HDR-pass PS (0x208a7ca00) moved in 1.007 to
        // 0x208a8e100. A stale skip silently does nothing, and blindly skipping
        // that address for another version can corrupt unrelated draws.
        string gameContentVersion = "";
        if (titleId == "PPSA01341" && File.Exists(paramFile))
        {
            try
            {
                using var param = JsonDocument.Parse(File.ReadAllText(paramFile));
                if (param.RootElement.TryGetProperty("contentVersion", out var version))
                    gameContentVersion = version.GetString() ?? "";
            }
            catch (JsonException) { /* Invalid game metadata: reject the shader skip. */ }
        }
        if (env.TryGetValue("KYTY_NATIVE_GPU_SKIP_PS", out var skipShader) &&
            !((gameContentVersion == "01.004.000" && skipShader == "0x208a7ca00") ||
              (gameContentVersion == "01.007.000" && skipShader == "0x208a8e100")))
            env.Remove("KYTY_NATIVE_GPU_SKIP_PS");

        if (cleanUpstream)
        {
            // Upstream is an untouched native backend: no overrides, shader skips,
            // KFD=0, GEM changes, workaround toggles, or frame-generation injection.
            foreach (var key in env.Keys.Where(key => key.StartsWith("KYTY_", StringComparison.Ordinal) ||
                key.StartsWith("LSFGVK_", StringComparison.Ordinal) || key == "ENABLE_LSFGVK").ToArray())
                env.Remove(key);
            // Only for the local binary selector; removed BEFORE executing official Kyty.
            env["KYTY_NATIVE_GPU_PM4_MODE"] = "upstream";
        }

        // Only the executable selector sees this recovery toggle. It chooses
        // the separately guarded build; the unmodified upstream ELF never
        // receives any experimental renderer overrides.
        if (titleId == "PPSA01341" && Value("KYTY_DEMONS_SCREAM_ASSERT_RECOVERY") == "1")
        {
            env["KYTY_DEMONS_SCREAM_ASSERT_RECOVERY"] = "1";
            env["KYTY_BC250_TITLE_ID"] = "PPSA01341";
        }
        // Set this AFTER stripping upstream renderer overrides. It is consumed
        // and removed by kyty-conditional-emu.sh before the native ELF starts.
        env["KYTY_SHARP_FG_MODE"] = fgMode;

        // Upscaling (after the upstream strip, like the FG mode). The output resolution is the game's own: Kyty
        // reports 4K or, for titles that allow resolution detection, 1080p from the window size. A choice here
        // replaces the display resolution above. The selector runs the native-upscaler build only when asked.
        // 3200x1800 and 2560x1440 are HLX's own output (the game still outputs 4K): HLX upscales to that size, the
        // preset is relative to it, and the native upscaler scales the result into the game's 4K image.
        var upscaleOutput = Value("upscale_output");
        var hlxOutput = upscaleOutput switch { "3200x1800" => "1800", "2560x1440" => "1440", _ => "" };
        if (upscaleOutput is "3840x2160" or "1920x1080" || hlxOutput.Length > 0)
        {
            var index = args.IndexOf("--screen-width");
            if (index >= 0 && index + 3 < args.Count) args.RemoveRange(index, 4);
            var size = (hlxOutput.Length > 0 ? "3840x2160" : upscaleOutput).Split('x');
            args.AddRange(["--screen-width", size[0], "--screen-height", size[1]]);
        }
        var upscaler = Value("upscaler");
        var preset = Value("upscale_preset");
        var kind = upscaler is "hlx_fsr4" or "hlx_dlss_e" or "auto" ? upscaler : "game";
        if (upscaler is "hlx_fsr4" or "hlx_dlss_e" or "auto" || hlxOutput.Length > 0 ||
            preset is "dlaa" or "ultra_quality" or "quality" or "balanced" or "performance" or "ultra_performance")
        {
            env["KYTY_NATIVE_UPSCALER_BUILD"] = "1";
            env["KYTY_UPSCALER"] = kind;
            env["KYTY_UPSCALE_PRESET"] = preset.Length > 0 ? preset : "dynamic";
            if (hlxOutput.Length > 0) env["KYTY_UPSCALE_OUTPUT"] = hlxOutput;
        }
        var framegen = Value("framegen");
        if (framegen is "fsr3" or "auto")
        {
            env["KYTY_NATIVE_UPSCALER_BUILD"] = "1";
            env.TryAdd("KYTY_UPSCALER", kind);
            env.TryAdd("KYTY_UPSCALE_PRESET", preset.Length > 0 ? preset : "dynamic");
            env["KYTY_FRAMEGEN"] = framegen;
        }
        else if (env.ContainsKey("KYTY_NATIVE_UPSCALER_BUILD"))
        {
            env["KYTY_FRAMEGEN"] = "off";
        }

        var exe = EmulatorPath;
        if (!File.Exists(exe)) throw new FileNotFoundException("BC-250 Kyty emulator executable not found", exe);
        var nice = Value("KYTY_BC250_CPU_NICE");
        var affinity = Value("KYTY_BC250_CPU_AFFINITY");
        if (affinity is "0-3" or "4-7" or "0-7" && File.Exists("/usr/bin/taskset"))
        {
            args.InsertRange(0, ["-c", affinity, exe]);
            exe = "/usr/bin/taskset";
        }
        if (nice is "5" or "10" && File.Exists("/usr/bin/nice"))
        {
            args.InsertRange(0, ["-n", nice, exe]);
            exe = "/usr/bin/nice";
        }
        // The selector needs the title for archived games too, before it strips renderer overrides.
        if (GameArchive.IsArchive(ebootPath) && !string.IsNullOrWhiteSpace(titleId))
            env["KYTY_BC250_TITLE_ID"] = titleId;
        return new Command(exe, args, env);
    }
}

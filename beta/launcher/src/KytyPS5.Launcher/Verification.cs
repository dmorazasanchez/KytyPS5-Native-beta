using System.Text.Json;

namespace KytyPS5;

internal static class Verification
{
    public static int Run()
    {
        var root = Path.Combine(Path.GetTempPath(), "kytyps5-verify-" + Guid.NewGuid().ToString("N"));
        var previous = Environment.GetEnvironmentVariable("KYTYPS5_PROFILE_ROOT");
        var previousConfig = Environment.GetEnvironmentVariable("KYTYPS5_CONFIG_ROOT");
        var previousBundle = Environment.GetEnvironmentVariable("KYTYPS5_BUNDLE_ROOT");
        Directory.CreateDirectory(root);
        // The generic checks run without the package's per-title defaults; a dedicated check covers them below.
        var testBundle = Path.Combine(root, "bundle");
        Environment.SetEnvironmentVariable("KYTYPS5_BUNDLE_ROOT", testBundle);
        Environment.SetEnvironmentVariable("KYTYPS5_PROFILE_ROOT", root);
        Environment.SetEnvironmentVariable("KYTYPS5_CONFIG_ROOT", Path.Combine(root, "config"));
        var passed = 0;
        void Check(bool condition, string name)
        {
            if (!condition) throw new InvalidOperationException("FAILED: " + name);
            Console.WriteLine("PASS: " + name); passed++;
        }
        try
        {
            var folder = Path.Combine(root, "Game with spaces & symbols"); Directory.CreateDirectory(Path.Combine(folder, "sce_sys"));
            var eboot = Path.Combine(folder, "eboot.bin"); File.WriteAllText(eboot, "fixture");
            var metadata = Path.Combine(folder, "sce_sys/param.json");
            File.WriteAllText(metadata, """{"titleId":"PPSA26344","contentVersion":"02.002.000","localizedParameters":{"en-US":{"titleName":"Test Game"}}}""");
            var game = Game.Read(eboot);
            Check(game.Name == "Test Game" && game.TitleId == "PPSA26344" && game.Version == "02.002.000", "PS5 metadata and paths with spaces");
            var settings = new Preferences { GameFolders = [root, folder, folder] };
            Check(Game.Scan(settings).Count == 1, "Nested library scan deduplicates installs");
            settings.ExcludedGames.Add(eboot); Check(Game.Scan(settings).Count == 0, "Excluded games remain excluded");
            Check(KytyBridge.Options.Select(o => o.Id).Distinct().Count() == KytyBridge.Options.Count, "All native setting IDs are unique");
            Check(KytyBridge.Options.Count == 75 && KytyBridge.Options.Any(o => o.Id == "upscale_preset"), "Complete native settings manifest including upscaling");
            KytyBridge.Save(null, null, new Dictionary<string, string> { ["resolution"] = "1920x1080", ["fg_mode"] = "adaptive", ["futureKey"] = "keep" });
            KytyBridge.Save(game.TitleId, eboot, new Dictionary<string, string> { ["KYTY_NATIVE_GPU_PM4_MODE"] = "upstream", ["upscaler"] = "hlx_fsr4", ["upscale_preset"] = "quality", ["upscale_output"] = "3840x2160" });
            var loaded = KytyBridge.Load(game.TitleId, eboot);
            Check(loaded["fg_mode"] == "adaptive" && loaded["futureKey"] == "keep" && loaded["upscaler"] == "hlx_fsr4", "Global inheritance, per-game overrides and unknown values");
            Check(KytyBridge.ProfileFile(game.TitleId, eboot) != KytyBridge.ProfileFile(game.TitleId, Path.Combine(root, "other/eboot.bin")), "Separate profiles for separate installs");
            Directory.CreateDirectory(Path.Combine(testBundle, "title-defaults"));
            var titleDefaults = Path.Combine(testBundle, "title-defaults", game.TitleId + ".json");
            File.WriteAllText(titleDefaults, """{"framegen":"fsr3","upscale_preset":"performance","upscaler":"game"}""");
            var layered = KytyBridge.Load(game.TitleId, eboot);
            Check(layered["fg_mode"] == "adaptive" && layered["framegen"] == "fsr3" && layered["upscale_preset"] == "quality" &&
                  layered["upscaler"] == "hlx_fsr4", "Shipped title defaults sit between global settings and the player's own choices");
            Check(KytyBridge.Load(game.TitleId, null)["framegen"] != "fsr3", "Title defaults never change the global settings page");
            File.Delete(titleDefaults);
            var command = KytyBridge.Build(eboot, game.TitleId);
            Check(command.Arguments.Contains(eboot) && command.Arguments.Count(a => a == eboot) == 1, "Game path remains a single argument");
            Check(command.Environment["KYTY_NATIVE_GPU_PM4_MODE"] == "upstream" && !command.Environment.ContainsKey("KYTY_NATIVE_GPU_KFD_QUEUES"), "Upstream renderer overrides stay filtered");
            Check(command.Environment["KYTY_SHARP_FG_MODE"] == "adaptive", "Frame generation survives upstream filtering");
            Check(command.Environment["KYTY_NATIVE_UPSCALER_BUILD"] == "1" && command.Environment["KYTY_UPSCALER"] == "hlx_fsr4" && command.Environment["KYTY_UPSCALE_PRESET"] == "quality", "Native upscaling survives upstream filtering");
            var width = command.Arguments.IndexOf("--screen-width");
            Check(command.Arguments[width + 1] == "3840" && command.Arguments[width + 3] == "2160", "Upscaling output replaces resolution exactly once");
            Check(!command.Environment.ContainsKey("KYTY_UPSCALE_OUTPUT"), "4K output needs no HLX output override");
            KytyBridge.Save(game.TitleId, eboot, new Dictionary<string, string> { ["KYTY_NATIVE_GPU_PM4_MODE"] = "upstream", ["upscaler"] = "hlx_fsr4", ["upscale_preset"] = "ultra_quality", ["upscale_output"] = "2560x1440" });
            var hlx1440 = KytyBridge.Build(eboot, game.TitleId);
            var width1440 = hlx1440.Arguments.IndexOf("--screen-width");
            Check(hlx1440.Environment["KYTY_UPSCALE_OUTPUT"] == "1440" && hlx1440.Environment["KYTY_UPSCALE_PRESET"] == "ultra_quality" &&
                  hlx1440.Arguments[width1440 + 1] == "3840" && hlx1440.Arguments[width1440 + 3] == "2160" &&
                  hlx1440.Arguments.Count(a => a == "--screen-width") == 1, "HLX 1440p output keeps the game at 4K");
            Check(hlx1440.Environment["KYTY_FRAMEGEN"] == "auto", "Native frame generation defaults to automatic (the selector applies it only to supported games)");
            KytyBridge.Save(game.TitleId, eboot, new Dictionary<string, string> { ["KYTY_NATIVE_GPU_PM4_MODE"] = "upstream", ["framegen"] = "fsr3" });
            var dyn = KytyBridge.Build(eboot, game.TitleId);
            Check(dyn.Environment["KYTY_FRAMEGEN"] == "fsr3" && dyn.Environment["KYTY_NATIVE_UPSCALER_BUILD"] == "1", "Native frame generation selects the native build");
            Check(dyn.Environment["KYTY_UPSCALER"] == "auto", "The upscaler defaults to automatic");
            KytyBridge.Save(game.TitleId, eboot, new Dictionary<string, string> { ["KYTY_NATIVE_GPU_PM4_MODE"] = "upstream", ["upscaler"] = "game", ["framegen"] = "off" });
            var off = KytyBridge.Build(eboot, game.TitleId);
            Check(!off.Environment.ContainsKey("KYTY_FRAMEGEN") && !off.Environment.ContainsKey("KYTY_NATIVE_UPSCALER_BUILD"),
                "Game upscaler with frame generation off keeps the production build");
            var demonsFolder = Path.Combine(root, "Demons Souls TAA");
            Directory.CreateDirectory(Path.Combine(demonsFolder, "sce_sys"));
            var demonsEboot = Path.Combine(demonsFolder, "eboot.bin");
            File.WriteAllText(demonsEboot, "fixture");
            File.WriteAllText(Path.Combine(demonsFolder, "sce_sys", "param.json"),
                """{"titleId":"PPSA01341","contentVersion":"01.007.000","localizedParameters":{"en-US":{"titleName":"Demon's Souls"}}}""");
            foreach (var kind in new[] { "hlx_fsr4", "hlx_dlss_e" })
            {
                KytyBridge.Save("PPSA01341", demonsEboot, new Dictionary<string, string> {
                    ["KYTY_NATIVE_GPU_PM4_MODE"] = "upstream", ["upscaler"] = kind, ["framegen"] = "fsr3", ["fg_mode"] = "off" });
                var taa = KytyBridge.Build(demonsEboot, "PPSA01341");
                Check(taa.Environment["KYTY_NATIVE_UPSCALER_BUILD"] == "1" && taa.Environment["KYTY_UPSCALER"] == kind &&
                      taa.Environment["KYTY_FRAMEGEN"] == "fsr3", "Demon's Souls TAA selects " + kind + " with native frame generation");
            }
            KytyBridge.Save("PPSA01341", demonsEboot, new Dictionary<string, string> {
                ["KYTY_NATIVE_GPU_PM4_MODE"] = "upstream", ["upscaler"] = "hlx_fsr4", ["framegen"] = "off" });
            var taaOff = KytyBridge.Build(demonsEboot, "PPSA01341");
            Check(taaOff.Environment["KYTY_FRAMEGEN"] == "off", "Native frame generation Off is explicit when Helix is selected");
            KytyBridge.Save("PPSA01341", demonsEboot, new Dictionary<string, string> {
                ["KYTY_NATIVE_GPU_PM4_MODE"] = "upstream", ["upscaler"] = "game", ["framegen"] = "fsr3" });
            var taaFg = KytyBridge.Build(demonsEboot, "PPSA01341");
            Check(taaFg.Environment["KYTY_UPSCALER"] == "game" && taaFg.Environment["KYTY_FRAMEGEN"] == "fsr3" &&
                  taaFg.Environment["KYTY_NATIVE_UPSCALER_BUILD"] == "1", "Demon's Souls native frame generation also works with Game TAA selected");
            var nativeSummary = ConsoleWindow.FrameGenerationSummary(new Dictionary<string, string> {
                ["fg_mode"] = "off", ["framegen"] = "fsr3" });
            Check(nativeSummary.Detail.Contains("FSR 3") && nativeSummary.Group == "Upscaling",
                "Dashboard shows native FSR 3 FG and opens its controls");
            var autoSummary = ConsoleWindow.FrameGenerationSummary(new Dictionary<string, string> {
                ["fg_mode"] = "off", ["framegen"] = "auto" });
            Check(autoSummary.Detail.StartsWith("Automatic") && autoSummary.Group == "Upscaling",
                "Dashboard shows Automatic native FG");
            var losslessSummary = ConsoleWindow.FrameGenerationSummary(new Dictionary<string, string> {
                ["fg_mode"] = "adaptive", ["framegen"] = "fsr3" });
            Check(losslessSummary.Detail.StartsWith("Lossless Scaling") && losslessSummary.Group == "Frame Generation",
                "Dashboard retains the selected Lossless Scaling controls");
            Check(UpscalerProfiles.Describe(null).StartsWith("Automatic", StringComparison.Ordinal), "Upscaling page explains automatic detection");
            var patch = Path.Combine(root, "cheats.json"); File.WriteAllText(patch, """{"id":"PPSA26344","version":"02.002.000","mods":[]}""");
            Check(KytyBridge.PatchMatchesGame(patch, eboot, game.TitleId), "Matching cheat title and version accepted");
            File.WriteAllText(patch, """{"id":"PPSA26344","version":"02.002.000","mods":[{"name":"Infinite Health","enabled":true,"memory":[{"offset":"0x100","on":"AA"}]}]}""", System.Text.Encoding.UTF8);
            var controls = new KytyCheatControls(game.TitleId, eboot, patch);
            var panel = (Avalonia.Controls.StackPanel)((Avalonia.Controls.ScrollViewer)controls.Tab.Content!).Content!;
            var toggle = panel.Children.OfType<Avalonia.Controls.CheckBox>().SingleOrDefault();
            Check(toggle is { IsChecked: true }, "Existing UTF-8 BOM cheat files populate the menu");
            toggle!.IsChecked = false;
            Check(controls.Save(out var cheatError), "BOM cheat selection saves: " + cheatError);
            using var savedCheats = JsonDocument.Parse(File.ReadAllBytes(patch));
            Check(!savedCheats.RootElement.GetProperty("mods")[0].GetProperty("enabled").GetBoolean() &&
                savedCheats.RootElement.GetProperty("mods")[0].GetProperty("memory")[0].GetProperty("on").GetString() == "AA",
                "Cheat toggle preserves patch instructions and writes readable UTF-8");
            var reopened = new KytyCheatControls(game.TitleId, eboot, patch);
            var reopenedPanel = (Avalonia.Controls.StackPanel)((Avalonia.Controls.ScrollViewer)reopened.Tab.Content!).Content!;
            Check(reopenedPanel.Children.OfType<Avalonia.Controls.CheckBox>().Single().IsChecked == false, "Saved cheat state reopens correctly");
            File.WriteAllText(patch, """{"id":"PPSA26344","version":"01.000.000","mods":[]}""");
            Check(!KytyBridge.PatchMatchesGame(patch, eboot, game.TitleId), "Wrong cheat version rejected");
            KytyBridge.Save(game.TitleId, eboot, new Dictionary<string, string> { ["KYTY_NATIVE_GPU_PM4_MODE"] = "fast", ["keymap"] = "Cross=Space\nCircle=B", ["extraArguments"] = "--test\nargument with spaces", ["KYTY_NATIVE_GPU_SKIP_PS"] = "0x208a8e100" });
            command = KytyBridge.Build(eboot, game.TitleId);
            Check(command.Arguments.Contains("argument with spaces") && command.Arguments.Count(a => a == "--keymap") == 2, "Custom arguments and controller mappings preserved without a shell");
            Check(!command.Environment.ContainsKey("KYTY_NATIVE_GPU_SKIP_PS"), "Demon's Souls shader skip never leaks to another title");
            GameArchiveVerification.Run(root, Check);
            GamepadVerification.Run(Check);
            AudioVerification.Run(Check);
            Console.WriteLine($"{passed} checks passed; {KytyBridge.Options.Count} native settings retained."); return 0;
        }
        catch (Exception e) { Console.Error.WriteLine(e); return 1; }
        finally
        {
            Environment.SetEnvironmentVariable("KYTYPS5_PROFILE_ROOT", previous);
            Environment.SetEnvironmentVariable("KYTYPS5_CONFIG_ROOT", previousConfig);
            Environment.SetEnvironmentVariable("KYTYPS5_BUNDLE_ROOT", previousBundle);
            Directory.Delete(root, true);
        }
    }
}

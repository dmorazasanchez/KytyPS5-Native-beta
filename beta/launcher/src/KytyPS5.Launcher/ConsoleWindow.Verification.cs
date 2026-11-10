using System.Text.Json;
using Avalonia;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;

namespace KytyPS5;

internal sealed partial class ConsoleWindow
{
    private async Task PreparePointerVerification(string directory)
    {
        if (Environment.GetEnvironmentVariable("KYTYPS5_CONFIG_ROOT") is null || Environment.GetEnvironmentVariable("KYTYPS5_PROFILE_ROOT") is null)
            throw new InvalidOperationException("Pointer verification requires isolated preferences and profiles.");
        Directory.CreateDirectory(directory); ShowSettings(null, "Audio"); await Task.Delay(150);
        Focus(_rows[0]); await Task.Delay(100);
        var sliders = _settingsContent.GetVisualDescendants().OfType<Avalonia.Controls.Slider>().ToArray();
        string? previous = null;
        void PublishCoordinates()
        {
            var coordinates = sliders.Select(slider => new {
                start = slider.PointToScreen(new Point(14, slider.Bounds.Height / 2)),
                end = slider.PointToScreen(new Point(slider.Bounds.Width - 14, slider.Bounds.Height / 2)) });
            var json = JsonSerializer.Serialize(coordinates);
            if (json == previous) return;
            previous = json; File.WriteAllText(System.IO.Path.Combine(directory, "coordinates.json"), json);
        }
        _settingsContent.LayoutUpdated += (_, _) => PublishCoordinates(); PublishCoordinates();
    }
    /// <summary>Opt-in UI integration checks. Requires isolated config and profile roots.</summary>
    private async Task RunUiSmoke(string directory)
    {
        var checks = new List<string>(); Directory.CreateDirectory(directory);
        void Check(bool condition, string name)
        {
            if (!condition) throw new InvalidOperationException(name);
            checks.Add(name); Console.WriteLine("PASS UI: " + name);
        }
        async Task Capture(string name)
        {
            await Task.Delay(150);
            using var bitmap = new RenderTargetBitmap(new PixelSize(1600, 900));
            bitmap.Render(_scene); bitmap.Save(System.IO.Path.Combine(directory, name + ".png"), new PngBitmapEncoderOptions());
        }
        try
        {
            Check(Environment.GetEnvironmentVariable("KYTYPS5_CONFIG_ROOT") is not null &&
                Environment.GetEnvironmentVariable("KYTYPS5_PROFILE_ROOT") is not null, "Isolated UI test state");
            if (Environment.GetEnvironmentVariable("KYTYPS5_INPUT_UI_CHECK") == "1")
            {
                await Task.Delay(1000);
                Check(IsActive, "Emulator window has keyboard focus");
                async Task Command(string command, params string[] arguments)
                {
                    var start = new System.Diagnostics.ProcessStartInfo(command) { UseShellExecute = false };
                    foreach (var argument in arguments) start.ArgumentList.Add(argument);
                    using var process = System.Diagnostics.Process.Start(start)!;
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                    Check(process.ExitCode == 0, System.IO.Path.GetFileName(command) + " completed");
                }
                await Command("/usr/bin/grim", System.IO.Path.Combine(directory, "display-home.png"));
                // Target this test's window explicitly. XTEST/EI injection needs
                // a desktop portal unavailable on this private headless bus.
                var windowId = TryGetPlatformHandle()!.Handle.ToInt64().ToString();
                var keyTool = Environment.GetEnvironmentVariable("KYTYPS5_KEY_TOOL")!;
                await Command(keyTool, windowId, "F10");
                await Task.Delay(500);
                Check(_modal is not null, "Keyboard event delivered through the display server opens the control centre");
                await Command("/usr/bin/grim", System.IO.Path.Combine(directory, "display-control-centre.png"));
                await Command(keyTool, windowId, "Escape");
                await Task.Delay(250);
                Check(_modal is null, "Keyboard event delivered through the display server closes the control centre");
                if (Environment.GetEnvironmentVariable("KYTYPS5_REQUIRE_PHYSICAL_PAD") == "1")
                    Check(_pad.Connected, "Physical controller discovered: " + _pad.Name);
                GamepadVerification.Run(Check);
                await Task.Delay(1200);
                Check(IsActive && _sessionTicks >= 3, "UI event loop remains responsive after keyboard/controller checks");
                File.WriteAllText(System.IO.Path.Combine(directory, "result.json"),
                    JsonSerializer.Serialize(new { passed = checks.Count, checks }, new JsonSerializerOptions { WriteIndented = true }));
                return;
            }
            if (Environment.GetEnvironmentVariable("KYTYPS5_SESSION_UI_CHECK") == "1")
            {
                ShowSystem(); await Capture("session-control-centre");
                var desktopButton = _modalRows.SelectMany(row => row).Single(button =>
                    button.Content is string label && label == "Return to Desktop Mode");
                Check(desktopButton is not null, "Control centre offers a desktop session switch");
                desktopButton!.RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
                await Task.Delay(100);
                var labels = _modal!.GetVisualDescendants().OfType<Avalonia.Controls.TextBlock>()
                    .Select(text => text.Text ?? "").ToArray();
                Check(labels.Any(text => text.Contains("KDE will open", StringComparison.Ordinal)),
                    "Desktop confirmation describes opening KDE");
                Check(!labels.Any(text => text.Contains("restart", StringComparison.OrdinalIgnoreCase) ||
                                         text.Contains("reboot", StringComparison.OrdinalIgnoreCase)),
                    "Desktop confirmation does not offer a reboot");
                Check(_modalRows.SelectMany(row => row).Any(button => button.Content is string label && label == "Switch to desktop"),
                    "Desktop confirmation offers Switch to desktop");
                await Capture("session-return-confirmation");
                // Exercise cancel only: executing the real action would end the host session.
                _modalBack?.Invoke();
                File.WriteAllText(System.IO.Path.Combine(directory, "result.json"),
                    JsonSerializer.Serialize(new { passed = checks.Count, checks }, new JsonSerializerOptions { WriteIndented = true }));
                return;
            }
            Check(_games.Count > 0, "Existing library imports");
            if (Environment.GetEnvironmentVariable("KYTYPS5_ARCHIVE_UI_CHECK") == "1")
            {
                _selected = _games.FindIndex(g => GameArchive.IsArchive(g.Path));
                Check(_selected >= 0, "An archive appears in the library");
                var archiveGame = Selected!;
                Check(archiveGame.TitleId == "PPSA21567" && archiveGame.Icon is not null && archiveGame.Background is not null,
                    "ASTRO BOT title, version and artwork load from the real archive");
                ShowHome(); await UpdateArtwork(); await Capture("archive-home");
                BrowseFolder(System.IO.Path.GetDirectoryName(archiveGame.Path)!, AddFolder);
                await Task.Delay(100);
                var archiveRow = _modalRows.SelectMany(row => row).OfType<Avalonia.Controls.Button>()
                    .Single(row => row.Content is string label && label.EndsWith(System.IO.Path.GetFileName(archiveGame.Path), StringComparison.Ordinal));
                Check(archiveRow is not null, "Add games browser displays the ZAR file");
                await Capture("archive-browser");
                archiveRow!.RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
                for (var i = 0; i < 30 && !_preferences.GameFolders.Contains(archiveGame.Path); i++) await Task.Delay(100);
                Check(_modal is null && _preferences.GameFolders.Contains(archiveGame.Path), "Selecting a ZAR file adds it directly");
                await ReloadLibrary();
                Check(_games.Count(g => g.Path == archiveGame.Path) == 1, "Folder and direct archive selections produce one game");
                _selected = _games.FindIndex(g => g.Path == archiveGame.Path);
                ShowLibrary(); await Capture("archive-library");
                var archiveRuntime = Environment.GetEnvironmentVariable("KYTY_SHARP_EMULATOR");
                Check(archiveRuntime is not null && archiveRuntime.StartsWith(directory, StringComparison.Ordinal), "Isolated archive runtime stub");
                ShowHome(); Launch();
                for (var i = 0; i < 50 && (_gameProcess is not null || _launching); i++) await Task.Delay(100);
                Check(_gameProcess is null && File.Exists(_logFile), "An archive launches through process supervision and returns to the library");
                var launchLog = File.ReadAllText(_logFile!);
                Check(launchLog.Contains(archiveGame.Path + "!/PPSA21567", StringComparison.Ordinal),
                    "The archive's internal game folder reaches the runtime unchanged");
                File.WriteAllText(System.IO.Path.Combine(directory, "result.json"), JsonSerializer.Serialize(new { passed = checks.Count, checks }, new JsonSerializerOptions { WriteIndented = true }));
                return;
            }
            _selected = Math.Max(0, _games.FindIndex(g => g.TitleId == "PPSA26344"));
            ShowHome(); await UpdateArtwork(); await Capture("home");
            Navigate(PadAction.Settings);
            await Task.Delay(100);
            Check(_page == "settings" && _settingsGame == Selected, "Triangle opens per-game settings");
            if (Selected?.TitleId == "PPSA26344")
            {
                ShowSettings(Selected, "Frame Generation"); await Task.Delay(100);
                Check(_values["fg_mode"] == "fixed" && _rows.Count == 1, "Yotei exposes the restored Lossless Scaling 2x selection");
                await Capture("lossless-scaling");
                ShowSettings(Selected, "Cheats"); await Task.Delay(100);
                Check(_rows.Count == 4, "All four existing Yotei cheat toggles load from the BOM-encoded file");
                await Capture("yotei-cheats");
                ShowSettings(null, "Cheats"); await Task.Delay(100); Navigate(PadAction.Right); Navigate(PadAction.Accept);
                Check(_settingsGame == Selected && _rows.Count == 4, "Global Cheats links directly to the selected game's cheat controls");
            }
            ShowSettings(Selected, "Upscaling"); await Task.Delay(100); Navigate(PadAction.Right);
            var original = _values["upscaler"];
            Navigate(PadAction.Right);
            Check(_values["upscaler"] != original, "D-pad changes and persists a setting");
            Navigate(PadAction.Left);
            Check(_values["upscaler"] == original, "D-pad reverses a choice");
            await Capture("upscaling");
            for (var i = 0; i < _groups.Length; i++)
            {
                SetCategory(i); await Task.Delay(50); Focus(_categories[i]); Navigate(PadAction.Right);
                foreach (var row in _rows.ToArray())
                {
                    Check(ReferenceEquals(FocusManager?.GetFocusedElement(), row), $"Controller reaches {_groups[i].Title} row {_rows.IndexOf(row) + 1}");
                    Navigate(PadAction.Down);
                }
            }
            ShowSettings(Selected, "Display"); await Capture("screen-video");
            ShowSettings(null, "Audio"); await Task.Delay(150);
            _preferences.Audio.MusicVolume = 15; _preferences.Audio.SoundsVolume = 65; SaveAudio(); SetCategory(_category); await Task.Delay(100);
            Focus(_rows[2]); Navigate(PadAction.Right);
            Check(_preferences.Audio.MusicVolume == 20 && _preferences.Audio.SoundsVolume == 65, "Controller adjusts music slider without changing effects volume");
            Navigate(PadAction.Left); Focus(_rows[4]); Navigate(PadAction.Left);
            Check(_preferences.Audio.MusicVolume == 15 && _preferences.Audio.SoundsVolume == 60, "Controller adjusts effects slider independently");
            Navigate(PadAction.Right);
            await Task.Delay(200);
            var restoredAudio = Preferences.Load().Audio;
            Check(restoredAudio.MusicVolume == 15 && restoredAudio.SoundsVolume == 65, "Both live slider values persist");
            var soundCount = _audio.Sounds.Emitted; _rows[0].RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            await Task.Delay(40);
            Check(_audio.Sounds.Emitted == soundCount + 1 && !_preferences.Audio.MusicEnabled && _preferences.Audio.SoundsEnabled,
                "Mouse click path toggles music alone and emits one sound");
            Focus(_rows[0]); await Task.Delay(130); soundCount = _audio.Sounds.Emitted;
            var key = new Avalonia.Input.KeyEventArgs { RoutedEvent = KeyDownEvent, Key = Avalonia.Input.Key.Enter };
            _rows[0].RaiseEvent(key); await Task.Delay(40);
            Check(key.Handled && _preferences.Audio.MusicEnabled && _audio.Sounds.Emitted == soundCount + 1,
                "Keyboard Enter uses the shared click path without duplicate effects");
            await Capture("audio-settings"); ShowMusicThemes(); await Capture("music-themes"); CloseModal();
            // Reopening settings must detach all controls from their previous visual tree.
            ShowSettings(null, "Display"); ShowSettings(Selected, "Display");
            Check(_settingsGame == Selected, "Repeated settings opens and scope changes");
            string? typed = null;
            EditText("Controller text entry", "test", value => typed = value, true, true);
            await Task.Delay(100);
            Navigate(PadAction.Accept); Navigate(PadAction.Next); Navigate(PadAction.Accept);
            Check(typed == "test1" && _modal is null, $"Controller keyboard enters and saves text (received: {typed ?? "<none>"})");
            EditText("Custom controller mapping", "Cross=Space\nCircle=B", _ => { }, true, true); await Capture("keyboard"); CloseModal();
            BrowseFolder(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), _ => { }); await Capture("folder-browser");
            Navigate(PadAction.Back); Check(_modal is null, "Controller closes folder browser");
            ShowLibrary(); await Capture("library");
            ShowSystem(); await Capture("control-centre"); Navigate(PadAction.Back);
            Check(_modal is null, "Controller closes control centre");
            // A stub runtime exercises process supervision and log handling without booting a game.
            var runtime = Environment.GetEnvironmentVariable("KYTY_SHARP_EMULATOR");
            Check(runtime is not null && runtime.StartsWith(directory, StringComparison.Ordinal), "Isolated runtime stub");
            ShowHome(); Launch();
            await Task.Delay(700);
            Check(_audio.Suspended, "Dashboard audio is suspended while the runtime stub is running");
            for (var i = 0; i < 50 && (_gameProcess is not null || _launching); i++) await Task.Delay(100);
            Check(_gameProcess is null && File.Exists(_logFile), "Session launch, log capture and return to launcher");
            Check(!_audio.Suspended, "Dashboard audio resumes after the game process exits");
            File.WriteAllText(System.IO.Path.Combine(directory, "result.json"), JsonSerializer.Serialize(new { passed = checks.Count, checks }, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception error)
        {
            Environment.ExitCode = 1;
            File.WriteAllText(System.IO.Path.Combine(directory, "failure.txt"), error.ToString()); Console.Error.WriteLine(error);
        }
        finally { _closing = true; Close(); }
    }
}

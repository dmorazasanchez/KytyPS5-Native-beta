using System.Diagnostics;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;

namespace KytyPS5;

internal sealed partial class ConsoleWindow
{
    private Game? _settingsGame;
    private readonly List<Button> _categories = [];
    private readonly List<Button> _rows = [];
    private readonly Dictionary<Button, Action<int>> _adjust = [];
    private ContentControl _settingsContent = new();
    private TextBlock _saveStatus = Text("Changes apply on the next launch.", 16, "#9FB2CA");
    private Button? _scopeButton;
    private Dictionary<string, string> _values = [];
    private int _category;
    private readonly (string Id, string Title, string Symbol, string Description)[] _groups = [
        ("Display", "Screen & video", "◫", "Resolution, refresh rate and the way your games are presented."),
        ("Upscaling", "Upscaling", "↗", "Choose the reconstruction method and balance image quality with performance."),
        ("Frame Generation", "Frame generation", "▥", "Smooth presentation with the existing Lossless Scaling profiles."),
        ("Audio", "Audio", "♪", "Background music and interface sounds. Changes take effect immediately."),
        ("BC-250 Renderer", "Native renderer", "◇", "Native GPU translation, memory and descriptor options."),
        ("GPU / Synchronization", "GPU & synchronization", "⇄", "Hardware queues, synchronization and version-specific workarounds."),
        ("CPU / Controller", "CPU, audio & controller", "⊕", "CPU scheduling, console identity, DualSense and input mapping."),
        ("Patches / Advanced", "Patches & diagnostics", "⌘", "Game patches, add-on content, logging and graphics diagnostics."),
        ("Cheats", "Cheats", "✧", "Version-checked cheats for the next game launch."),
        ("Extra", "Advanced overrides", "≡", "Additional launch arguments and environment variables."),
        ("Launcher", "Appearance & library", "◉", "Personalize KytyPS5 and manage your game folders.") ];

    private void ShowSettings(Game? game, string? group = null)
    {
        _audio.Sounds.Request(UiSound.Open);
        _settingsContent = new();
        _saveStatus = Text("Changes apply on the next launch.", 16, "#9FB2CA");
        _page = "settings"; _settingsGame = game; _backdrop.Opacity = .12;
        if (group is not null) _category = Math.Max(0, Array.FindIndex(_groups, g => g.Id == group));
        _values = KytyBridge.Load(game?.TitleId, game?.Path);
        _categories.Clear();
        var page = new Grid { RowDefinitions = new("106,*,42"), Margin = new(68, 6, 68, 0) };
        var header = new Grid { ColumnDefinitions = new("*,Auto"), ColumnSpacing = 20 };
        var titles = new StackPanel { Spacing = 5 };
        titles.Children.Add(Text("Settings", 42, weight: FontWeight.Light));
        titles.Children.Add(Text(game is null ? "Global defaults  /  Used by games without an override" : game.Name + "  /  Per-game profile", 18, "#A7B9CE"));
        header.Children.Add(titles);
        var scope = Button(game is null && Selected is not null ? "Selected game  →" : "Global defaults  →", () => ShowSettings(game is null ? Selected : null));
        _scopeButton = scope;
        scope.IsEnabled = game is not null || Selected is not null; scope.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(scope, 1); header.Children.Add(scope); page.Children.Add(header);
        var columns = new Grid { ColumnDefinitions = new("345,*"), ColumnSpacing = 38 };
        var navigation = new StackPanel { Spacing = 5 };
        for (var i = 0; i < _groups.Length; i++)
        {
            var index = i;
            var button = Button(_groups[i].Symbol + "   " + _groups[i].Title, () => { SetCategory(index); Focus(_rows.FirstOrDefault()); }, "nav");
            button.FontSize = 20; button.MinHeight = 48; button.Padding = new(20, 9);
            _categories.Add(button); navigation.Children.Add(button);
        }
        var navScroll = new ScrollViewer { Content = navigation };
        columns.Children.Add(navScroll); Grid.SetColumn(_settingsContent, 1); columns.Children.Add(_settingsContent);
        Grid.SetRow(columns, 1); page.Children.Add(columns);
        var footer = new Grid { ColumnDefinitions = new("*,Auto") };
        footer.Children.Add(_saveStatus);
        var back = Button("←  Back to games", () => OpenPage("home"), "nav"); back.FontSize = 17; back.MinHeight = 36; back.Padding = new(10, 0);
        Grid.SetColumn(back, 1); footer.Children.Add(back); Grid.SetRow(footer, 2); page.Children.Add(footer);
        _body.Content = page; SetCategory(_category); Focus(_categories[_category]);
        for (var i = 0; i < _tabs.Count; i++) _tabs[i].Classes.Set("active", i == 2);
        UpdateHints();
    }

    private void SetCategory(int index)
    {
        _category = Math.Clamp(index, 0, _groups.Length - 1);
        for (var i = 0; i < _categories.Count; i++) _categories[i].Classes.Set("active", i == _category);
        _rows.Clear(); _adjust.Clear();
        var group = _groups[_category];
        var content = new StackPanel { Spacing = 10, Margin = new(0, 4, 12, 24) };
        content.Children.Add(Text(group.Title, 32, weight: FontWeight.Light));
        content.Children.Add(new TextBlock { Text = group.Description, FontSize = 17, Foreground = Brush("#A7B9CE"), TextWrapping = TextWrapping.Wrap, Margin = new(0, 0, 0, 16) });
        if (group.Id == "Upscaling")
            content.Children.Add(Note(UpscalerProfiles.Describe(_settingsGame)));
        if (group.Id == "Frame Generation")
            content.Children.Add(Note("Lossless Scaling: Fixed uses the verified 2× profile. Adaptive targets 60 FPS. Works with the native upscaling build. Changes apply at the next game launch."));
        foreach (var option in KytyBridge.Options.Where(o => o.Group == group.Id))
        {
            var description = Describe(option.Id);
            var value = _values.GetValueOrDefault(option.Id, option.Default);
            var row = Setting(option.Label, ChoiceLabel(option.Id, value), description);
            if (option.Choices is { Length: > 0 } choices)
            {
                void Adjust(int direction)
                {
                    using var operation = _audio.Sounds.Operation();
                    var current = _values.GetValueOrDefault(option.Id, option.Default);
                    var choice = Array.IndexOf(choices, current);
                    var next = choices[(Math.Max(0, choice) + direction + choices.Length) % choices.Length];
                    if (SaveValue(option.Id, next))
                    {
                        UpdateRowValue(row, ChoiceLabel(option.Id, next));
                        _audio.Sounds.Request(option.Boolean || option.Choices.Length == 2 && option.Choices.All(c => c is "0" or "1")
                            ? next == "1" ? UiSound.Enable : UiSound.Disable : UiSound.Confirm);
                    }
                }
                _adjust[row] = Adjust; row.Click += (_, _) => Adjust(1);
            }
            else row.Click += (_, _) => EditOption(option.Id, option.Label);
            content.Children.Add(row);
        }
        if (group.Id == "Cheats") AddCheats(content);
        if (group.Id == "Audio") AddAudioSettings(content);
        if (group.Id == "Extra")
        {
            foreach (var (id, label, description) in new[] {
                ("extraArguments", "Extra command arguments", "One argument per line. Available in the Fast profile."),
                ("extraEnvironment", "Extra environment variables", "One KEY=VALUE per line. Available in the Fast profile.") })
            {
                var row = Setting(label, string.IsNullOrEmpty(_values.GetValueOrDefault(id)) ? "Not set" : "Edit", description);
                row.Click += (_, _) => EditOption(id, label); content.Children.Add(row);
            }
        }
        if (group.Id == "Launcher") AddLauncherSettings(content);
        if (_settingsGame is not null && group.Id == "Extra")
        {
            var reset = Setting("Use global defaults", "Reset profile", "Remove this game's overrides. A backup is retained.");
            reset.Click += (_, _) => Confirm("Reset game profile?", "This game will use global defaults. The current profile will be backed up.", "Reset profile", () =>
            {
                var path = KytyBridge.ProfileFile(_settingsGame.TitleId, _settingsGame.Path);
                if (File.Exists(path)) File.Move(path, path + ".backup-" + DateTime.Now.ToString("yyyyMMdd-HHmmssfff"));
                _values = KytyBridge.Load(_settingsGame.TitleId, _settingsGame.Path); SetCategory(_category); Focus(_rows.FirstOrDefault());
            });
            content.Children.Add(reset);
        }
        _settingsContent.Content = new ScrollViewer { Content = content, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
    }

    private static Border Note(string message) => new() { Background = Brush("#162B40"), CornerRadius = new(8), Padding = new(20, 16), Margin = new(0, 0, 0, 9), Child = Text(message, 16, "#BED9F4") };

    private Button Setting(string title, string value, string description = "")
    {
        var grid = new Grid { ColumnDefinitions = new("*,290"), ColumnSpacing = 24 };
        var labels = new StackPanel { Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        labels.Children.Add(Text(title, 22));
        if (description.Length > 0) labels.Children.Add(Text(description, 15, "#A4B5CA"));
        grid.Children.Add(labels);
        var state = Text(value + "  ›", 21, "#BBDFFF", FontWeight.Medium);
        state.TextAlignment = TextAlignment.Right; Grid.SetColumn(state, 1); grid.Children.Add(state);
        var row = Button("", () => { }, "setting"); row.Content = grid; row.Tag = state;
        _rows.Add(row); return row;
    }
    private static void UpdateRowValue(Button row, string value) { if (row.Tag is TextBlock text) text.Text = value + "  ›"; }

    private bool SaveValue(string key, string value)
    {
        try
        {
            var path = KytyBridge.ProfileFile(_settingsGame?.TitleId, _settingsGame?.Path);
            var stored = File.Exists(path) ? JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(path)) ?? [] : [];
            stored[key] = value;
            KytyBridge.Save(_settingsGame?.TitleId, _settingsGame?.Path, stored);
            _values[key] = value; _saveStatus.Text = "✓  Saved  /  Applies on the next launch";
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            Message("Could not save setting", e.Message); return false;
        }
    }

    private void EditOption(string id, string title)
    {
        EditText(title, _values.GetValueOrDefault(id, ""), value =>
        {
            SaveValue(id, value); SetCategory(_category); Focus(_rows.FirstOrDefault());
        }, id is "keymap" or "extraArguments" or "extraEnvironment",
            id is "patch" or "addcont" or "shaderFolder" or "bufferFolder" or "printfFolder");
    }

    private void AddCheats(StackPanel content)
    {
        if (_settingsGame is null && Selected is { } selected)
        {
            content.Children.Add(Note("Cheats are configured per game. Open the selected game's cheat controls below."));
            var manage = Setting(selected.Name, "Manage cheats", selected.TitleId + "  /  " + selected.Version);
            manage.Click += (_, _) => ShowSettings(selected, "Cheats"); content.Children.Add(manage); return;
        }
        var controls = new KytyCheatControls(_settingsGame?.TitleId, _settingsGame?.Path, _values.GetValueOrDefault("patch"));
        var stack = (controls.Tab.Content as ScrollViewer)?.Content as StackPanel;
        if (stack is null) return;
        var choices = stack.Children.OfType<CheckBox>().ToList();
        if (choices.Count == 0)
        {
            foreach (var text in stack.Children.OfType<TextBlock>().Skip(1)) content.Children.Add(Note(text.Text ?? ""));
            return;
        }
        content.Children.Add(Note("Cheat changes are saved for the next launch. Multi-byte hooks require a full game restart."));
        foreach (var check in choices)
        {
            var row = Setting(check.Content?.ToString() ?? "Cheat", check.IsChecked == true ? "On" : "Off");
            void Toggle(int direction)
            {
                using var operation = _audio.Sounds.Operation();
                check.IsChecked = check.IsChecked != true;
                _audio.Sounds.Request(check.IsChecked == true ? UiSound.Enable : UiSound.Disable);
                if (!controls.Save(out var error)) { Message("Could not save cheats", error); return; }
                var index = _rows.IndexOf(row); SetCategory(_category); Focus(_rows.ElementAtOrDefault(index));
            }
            row.Click += (_, _) => Toggle(1); _adjust[row] = Toggle; content.Children.Add(row);
        }
    }

    private void AddLauncherSettings(StackPanel content)
    {
        void Choice(string title, Func<string> read, Action<int> change, string description = "")
        {
            var row = Setting(title, read(), description);
            void Adjust(int direction)
            {
                using var operation = _audio.Sounds.Operation();
                change(direction); _preferences.Save(); UpdateRowValue(row, read());
                _audio.Sounds.Request(read() == "On" ? UiSound.Enable : read() == "Off" ? UiSound.Disable : UiSound.Confirm);
            }
            row.Click += (_, _) => Adjust(1); _adjust[row] = Adjust; content.Children.Add(row);
        }
        Choice("Launcher theme", () => _preferences.Theme, direction =>
        {
            var themes = new[] { "Midnight", "Graphite", "OLED" };
            _preferences.Theme = themes[(Array.IndexOf(themes, _preferences.Theme) + direction + 3) % 3]; ApplyTheme();
        });
        Choice("Fullscreen launcher", () => _preferences.Fullscreen ? "On" : "Off", _ =>
        {
            _preferences.Fullscreen = !_preferences.Fullscreen; WindowState = _preferences.Fullscreen ? WindowState.FullScreen : WindowState.Maximized;
        }, "F11 also toggles fullscreen.");
        Choice("Game title music", () => _preferences.PlayTitleMusic ? "On" : "Off", _ => { _preferences.PlayTitleMusic = !_preferences.PlayTitleMusic; PlayMusic(); });
        Choice("Title music volume", () => _preferences.MusicVolume + "%", direction => { _preferences.MusicVolume = Math.Clamp(_preferences.MusicVolume + direction * 5, 0, 100); PlayMusic(); });
        var add = Setting("Add a game folder", "Browse", "Choose a game install or a folder containing game installs.");
        add.Click += (_, _) => BrowseFolder(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), AddFolder); content.Children.Add(add);
        var refresh = Setting("Refresh library", "Scan folders"); refresh.Click += async (_, _) => await ReloadLibrary(); content.Children.Add(refresh);
        foreach (var folder in _preferences.GameFolders)
        {
            var row = Setting(System.IO.Path.GetFileName(folder), "Remove folder", folder);
            row.Click += (_, _) => Confirm("Remove library folder?", "The game files will stay on disk.\n" + folder, "Remove folder", async () => { _preferences.GameFolders.Remove(folder); _preferences.Save(); await ReloadLibrary(); });
            content.Children.Add(row);
        }
    }

    private void ApplyTheme()
    {
        _scene.Background = Brush(_preferences.Theme == "OLED" ? "#000000" : _preferences.Theme == "Graphite" ? "#191C22" : "#080D16");
        if (_preferences.Theme == "OLED") _backdrop.Opacity = _page == "settings" ? .05 : .65;
    }

    private void NavigateSettings(PadAction action)
    {
        if (action is PadAction.Previous or PadAction.Next)
        {
            SetCategory(Math.Clamp(_category + (action == PadAction.Next ? 1 : -1), 0, _groups.Length - 1)); Focus(_categories[_category]); return;
        }
        var focused = FocusManager?.GetFocusedElement() as Button;
        var nav = _categories.IndexOf(focused!);
        var index = _rows.IndexOf(focused!);
        if (action == PadAction.Back)
        {
            if (index >= 0) Focus(_categories[_category]); else OpenPage("home");
            return;
        }
        if (nav >= 0)
        {
            if (action is PadAction.Up or PadAction.Down)
            {
                if (nav == 0 && action == PadAction.Up) { Focus(_scopeButton); return; }
                SetCategory(Math.Clamp(nav + (action == PadAction.Down ? 1 : -1), 0, _groups.Length - 1)); Focus(_categories[_category]);
            }
            else if (action is PadAction.Accept or PadAction.Right) Focus(_rows.FirstOrDefault());
            else if (action == PadAction.Left) Focus(_tabs[2]);
        }
        else if (index >= 0)
        {
            if (action is PadAction.Up or PadAction.Down) Focus(_rows[Math.Clamp(index + (action == PadAction.Down ? 1 : -1), 0, _rows.Count - 1)]);
            else if (action is PadAction.Left or PadAction.Right)
            {
                if (_adjust.TryGetValue(focused!, out var adjust)) adjust(action == PadAction.Right ? 1 : -1);
                else if (action == PadAction.Left) Focus(_categories[_category]);
            }
            else if (action == PadAction.Accept) focused?.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
        }
        else
        {
            if (action == PadAction.Accept) focused?.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            else if (action == PadAction.Down || action == PadAction.Right) Focus(_categories[_category]);
            else if (action == PadAction.Left) Focus(_tabs[1]);
        }
    }

    internal static (string Detail, string Group) FrameGenerationSummary(IReadOnlyDictionary<string, string> values)
    {
        var lossless = values.GetValueOrDefault("fg_mode", "off");
        return lossless is "fixed" or "adaptive"
            ? ("Lossless Scaling  ·  " + ChoiceLabel("fg_mode", lossless), "Frame Generation")
            : (ChoiceLabel("framegen", values.GetValueOrDefault("framegen", "auto")), "Upscaling");
    }

    private static string ChoiceLabel(string id, string value) => (id, value) switch
    {
        ("upscaler", "auto") => "Automatic  ·  HLX / FSR 4 when supported",
        ("upscaler", "game") => "Game default", ("upscaler", "hlx_fsr4") => "HLX / FSR 4", ("upscaler", "hlx_dlss_e") => "HLX / DLSS (Model E)",
        ("upscale_preset", "dynamic") => "Dynamic", ("upscale_preset", "dlaa") => "Native / DLAA",
        ("upscale_preset", "ultra_quality") => "Ultra quality  ·  1.3×",
        ("upscale_preset", "quality") => "Quality  ·  1.5×", ("upscale_preset", "balanced") => "Balanced  ·  1.7×",
        ("upscale_preset", "performance") => "Performance  ·  2×", ("upscale_preset", "ultra_performance") => "Ultra performance  ·  3×",
        ("upscale_output", "display") => "Use screen resolution",
        ("upscale_output", "3200x1800") => "3200 × 1800  ·  HLX, shown at 4K", ("upscale_output", "2560x1440") => "2560 × 1440  ·  HLX, shown at 4K",
        ("framegen", "off") => "Off", ("framegen", "fsr3") => "FSR 3  ·  2×", ("framegen", "auto") => "Automatic  ·  FSR 3 2× when supported",
        ("fg_mode", "off") => "Off", ("fg_mode", "fixed") => "Fixed 2×", ("fg_mode", "adaptive") => "Adaptive 60 FPS",
        ("KYTY_NATIVE_GPU_PM4_MODE", "fast") => "Fast", ("KYTY_NATIVE_GPU_PM4_MODE", "upstream") => "Upstream",
        ("KYTY_BC250_AUDIO_TEST", "off") => "Original mix", ("KYTY_BC250_AUDIO_TEST", "dialogue") => "Dialogue test",
        ("language", "0") => "Japanese", ("language", "1") => "English (US)", ("language", "2") => "French",
        ("language", "3") => "Spanish", ("language", "4") => "German", ("language", "5") => "Italian",
        _ => KytyBridge.Options.FirstOrDefault(o => o.Id == id)?.Boolean == true ? (value == "1" ? "On" : "Off") : string.IsNullOrEmpty(value) ? "Not set" : value,
    };

    private static string Describe(string id) => id switch
    {
        "upscaler" => "Automatic: HLX / FSR 4 replaces the game's upscaler once Kyty has verified its inputs (games whose interface was identified); other games keep theirs. Or force the game's reconstruction or an HLX path. Choose it here: switching upscalers during play can crash the system on the BC-250 when memory is tight.",
        "framegen" => "FSR 3.1 adds a generated frame between real frames using the game's depth and motion. Automatic enables it below 50 real FPS. It pauses when shared memory is low. Switch in game: PS button > Upscaling.",
        "upscale_preset" => "Native renders at output resolution. Higher ratios reduce internal resolution. Ratios are relative to the output resolution. Changes apply at the next game launch.",
        "upscale_output" => "Overrides the screen resolution when set explicitly. 1800p / 1440p: HLX upscales to that size (less GPU time and memory) and the result is scaled to the game's 4K; the game's FSR3 stays at 4K.",
        "KYTY_NATIVE_GPU_PM4_MODE" => "Upstream uses the validated native build. Fast retains the experimental options.",
        "KYTY_NATIVE_GPU_SKIP_PS" => "Demon's Souls only. Checked against the exact installed game version.",
        "KYTY_DEMONS_SCREAM_ASSERT_RECOVERY" => "Selects the separate recovery build for Demon's Souls.",
        "keymap" => "One Control=Input mapping per line.",
        "patch" => "Only a JSON patch matching this game's title ID and version is applied.",
        "volume" => "DualSense built-in speaker level.", "vibration" => "DualSense vibration strength.",
        "fg_mode" => "Presentation only. Fixed 2× or adaptive 60 FPS.",
        _ => "",
    };
}

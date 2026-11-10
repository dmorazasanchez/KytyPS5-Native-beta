using System.Diagnostics;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;

namespace KytyPS5;

internal sealed partial class ConsoleWindow : Window
{
    private readonly Preferences _preferences = Preferences.Load();
    private readonly Gamepad _pad = new();
    private readonly AudioEngine _audio = new();
    private readonly DispatcherTimer _inputTimer = new() { Interval = TimeSpan.FromMilliseconds(33) };
    private readonly Grid _scene = new();
    private readonly Image _backdrop = new() { Stretch = Stretch.UniformToFill };
    private readonly ContentControl _body = new();
    private readonly TextBlock _hints = Text("", 18, "#D4DFED");
    private readonly TextBlock _clock = Text("", 20);
    private readonly TextBlock _connection = Text("", 15, "#9EAEC1");
    private readonly List<Button> _tabs = [];
    private readonly List<Button> _tiles = [];
    private readonly List<Button> _actions = [];
    private readonly Dictionary<string, Bitmap> _icons = [];
    private Bitmap? _backgroundBitmap;
    private List<Game> _games = [];
    private int _selected;
    private int _artGeneration;
    private string _page = "home";
    private Process? _gameProcess;
    private bool _launching;
    private readonly Queue<string> _logLines = new();
    private string? _logFile;
    private long _clockAt;
    private string? _inputStatus;
    private bool _closing;
    private long _focusSequence;
    private Game? Selected => _games.ElementAtOrDefault(_selected);

    public ConsoleWindow()
    {
        _audio.Configure(_preferences.Audio);
        _audio.Sounds.Schedule = callback => Dispatcher.UIThread.Post(callback, DispatcherPriority.Input);
        Title = "KytyPS5";
        Width = 1440; Height = 810; MinWidth = 960; MinHeight = 540;
        Background = Brush("#080D16");
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        WindowState = _preferences.Fullscreen || Environment.GetEnvironmentVariable("PS5_MINIMAL_SESSION") == "1"
            ? WindowState.FullScreen : WindowState.Maximized;
        Icon = new WindowIcon(Avalonia.Platform.AssetLoader.Open(new Uri("avares://KytyPS5/Assets/kytyps5.png")));
        var layout = new Grid { RowDefinitions = new("100,*,70") };
        _scene.Width = 1600; _scene.Height = 900;
        _scene.Children.Add(_backdrop);
        _scene.Children.Add(new Border { Background = new LinearGradientBrush {
            StartPoint = new RelativePoint(0, .5, RelativeUnit.Relative), EndPoint = new RelativePoint(1, .5, RelativeUnit.Relative),
            GradientStops = new() { new(Color.Parse("#ED080D16"), 0), new(Color.Parse("#A3080D16"), .45), new(Color.Parse("#28080D16"), 1) } } });
        _scene.Children.Add(new Border { Background = new LinearGradientBrush {
            StartPoint = new RelativePoint(.5, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(.5, 1, RelativeUnit.Relative),
            GradientStops = new() { new(Color.Parse("#A0080D16"), 0), new(Color.Parse("#00080D16"), .38), new(Color.Parse("#F5080D16"), 1) } } });
        _scene.Children.Add(layout);
        var header = new Grid { ColumnDefinitions = new("*,Auto"), Margin = new(58, 18, 58, 14) };
        var navigation = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        foreach (var (label, page) in new[] { ("Games", "home"), ("Library", "library"), ("Settings", "settings") })
        {
            var button = Button(label, () => OpenPage(page), "top");
            _tabs.Add(button); navigation.Children.Add(button);
        }
        header.Children.Add(navigation);
        var brand = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 28, VerticalAlignment = VerticalAlignment.Center };
        brand.Children.Add(_connection);
        brand.Children.Add(Text("KytyPS5", 26, weight: FontWeight.SemiBold));
        brand.Children.Add(_clock);
        Grid.SetColumn(brand, 1); header.Children.Add(brand); layout.Children.Add(header);
        Grid.SetRow(_body, 1); layout.Children.Add(_body);
        var footer = new Grid { ColumnDefinitions = new("*,Auto"), Margin = new(68, 12, 68, 20) };
        footer.Children.Add(_hints);
        var system = Button("☰   Control centre", ShowSystem, "nav");
        system.FontSize = 17; system.MinHeight = 38; system.Padding = new(14, 4);
        Grid.SetColumn(system, 1); footer.Children.Add(system); Grid.SetRow(footer, 2); layout.Children.Add(footer);
        Content = new Viewbox { Stretch = Stretch.Uniform, Child = _scene };
        AddHandler(KeyDownEvent, OnInputKey, RoutingStrategies.Tunnel);
        _inputTimer.Tick += (_, _) =>
        {
            foreach (var action in _pad.Poll(IsActive)) Navigate(action);
            var status = _pad.Connected ? _pad.Name : "Keyboard ready";
            if (_inputStatus != status) { _inputStatus = status; _connection.Text = status.Length > 28 ? "Controller connected" : status; UpdateHints(); }
            if (Environment.TickCount64 > _clockAt) { _clockAt = Environment.TickCount64 + 15000; _clock.Text = DateTime.Now.ToString("HH:mm"); }
            RefreshAudioStatus();
            PublishSessionHealth();
        };
        Opened += async (_, _) =>
        {
            _inputTimer.Start(); await ReloadLibrary(); ApplyTheme(); _libraryLoaded = true;
            if (Environment.GetEnvironmentVariable("PS5_MINIMAL_SESSION") == "1") Activate();
            if (Environment.GetEnvironmentVariable("KYTYPS5_UI_SMOKE_DIR") is { } directory) await RunUiSmoke(directory);
            else if (Environment.GetEnvironmentVariable("KYTYPS5_POINTER_TEST_DIR") is { } pointerDirectory) await PreparePointerVerification(pointerDirectory);
        };
        Closing += (_, e) =>
        {
            if (_gameProcess is not null && !_closing)
            {
                e.Cancel = true;
                Confirm("A game is running", "Stop the game and close KytyPS5?", "Stop & exit", () => { StopGame(); _closing = true; Close(); });
            }
        };
        Closed += (_, _) =>
        {
            _closing = true;
            _inputTimer.Stop(); _audioSaveTimer?.Dispose(); _pad.Dispose(); _audio.Dispose(); _backgroundBitmap?.Dispose();
            foreach (var icon in _icons.Values) icon.Dispose();
            _preferences.Save();
        };
        ShowHome();
    }

    private static SolidColorBrush Brush(string color) => new(Color.Parse(color));
    private static TextBlock Text(string text, double size = 20, string color = "#F5F6FA", FontWeight? weight = null) => new()
    {
        Text = text, FontSize = size, Foreground = Brush(color), FontWeight = weight ?? FontWeight.Normal,
        TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center,
    };
    private Button Button(string label, Action action, string style = "")
    {
        var button = new Button { Content = label };
        if (style.Length > 0) button.Classes.Add(style);
        // All input methods raise the same Click event; navigation operations coalesce semantic feedback.
        button.Click += (_, _) =>
        {
            using var operation = _audio.Sounds.Operation();
            if (!button.Classes.Contains("audioPreview"))
                _audio.Sounds.Request(label is "Back" or "Cancel" || label.StartsWith("←", StringComparison.Ordinal) ? UiSound.Back : UiSound.Confirm);
            action();
        };
        button.GotFocus += (_, _) => _audio.Sounds.Request(UiSound.Move);
        button.PointerEntered += (_, _) => _audio.Sounds.Request(UiSound.Move);
        return button;
    }
    private void Focus(Control? control)
    {
        var request = ++_focusSequence;
        if (control is null) return;
        if (!control.Focus(NavigationMethod.Directional))
            Dispatcher.UIThread.Post(() =>
            {
                if (request != _focusSequence || _closing) return;
                control.Focus(NavigationMethod.Directional); control.BringIntoView();
            }, DispatcherPriority.Loaded);
        else control.BringIntoView();
    }

    private async Task ReloadLibrary()
    {
        _games = await Task.Run(() => Game.Scan(_preferences));
        UpscalerProfiles.ScanInBackground(_games); // games added or found: identify their upscaler interface
        _selected = Math.Max(0, _games.FindIndex(g => g.Path == _preferences.LastGame));
        OpenPage(_page == "settings" ? "settings" : _page);
        await UpdateArtwork();
    }

    private void OpenPage(string page)
    {
        if (_modal is not null) return;
        if (_page != page) _audio.Sounds.Request(UiSound.Open);
        _page = page;
        for (var i = 0; i < _tabs.Count; i++) _tabs[i].Classes.Set("active", i == (page == "home" ? 0 : page == "library" ? 1 : 2));
        if (page == "settings") ShowSettings(null);
        else if (page == "library") ShowLibrary();
        else ShowHome();
        UpdateHints();
    }

    private void ShowHome()
    {
        _page = "home"; _actions.Clear(); _tiles.Clear();
        for (var i = 0; i < _tabs.Count; i++) _tabs[i].Classes.Set("active", i == 0);
        _backdrop.Opacity = 1;
        var page = new Grid { RowDefinitions = new("210,*,160"), Margin = new(68, 8, 68, 0) };
        var rail = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 20, Margin = new(0, 4, 0, 10) };
        for (var i = 0; i < _games.Count; i++)
        {
            var index = i;
            var tile = MakeTile(_games[i], 164, false, () => SelectGame(index));
            _tiles.Add(tile); rail.Children.Add(tile); tile.Classes.Set("selected", i == _selected);
            tile.GotFocus += (_, _) => { if (_selected != index) SelectGame(index, false); };
        }
        var add = Button("+", () => BrowseFolder(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), AddFolder), "tile");
        add.Width = 100; add.Height = 164; add.FontSize = 42; rail.Children.Add(add); _tiles.Add(add);
        var railScroll = new ScrollViewer { Content = rail, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Hidden,
            VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
        page.Children.Add(railScroll);
        var hero = new StackPanel { Spacing = 19, VerticalAlignment = VerticalAlignment.Center, MaxWidth = 940, HorizontalAlignment = HorizontalAlignment.Left };
        hero.Children.Add(Text(Selected is null ? "WELCOME TO KYTYPS5" : "PS5  /  " + Selected.TitleId, 16, "#A9BDD4", FontWeight.SemiBold));
        hero.Children.Add(Text(Selected?.Name ?? "Your games. Your console.", 58, weight: FontWeight.Light));
        hero.Children.Add(Text(Selected is null ? "Add your game folders to get started." : $"Version {Selected.Version}   •   Native PS5 runtime", 20, "#BEC9D7"));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16, Margin = new(0, 12, 0, 0) };
        var play = Button(_gameProcess is null ? "▶   Play game" : "Game running", Launch, "primary");
        play.Width = 300; play.Height = 66; play.IsEnabled = Selected is not null && _gameProcess is null && !_launching;
        var options = Button("Game settings", () => ShowSettings(Selected), "nav");
        options.Width = 240; options.Height = 66; options.IsEnabled = Selected is not null;
        buttons.Children.Add(play); buttons.Children.Add(options); _actions.Add(play); _actions.Add(options);
        hero.Children.Add(buttons); Grid.SetRow(hero, 1); page.Children.Add(hero);
        var quick = new Grid { ColumnDefinitions = new("*,*,*"), ColumnSpacing = 18, Margin = new(0, 24, 0, 8) };
        var values = Selected is { } game ? KytyBridge.Load(game.TitleId, game.Path) : KytyBridge.Load(null, null);
        var quickData = new[] {
            ("01", "Upscaling", ChoiceLabel("upscaler", values["upscaler"]) + "  ·  " + ChoiceLabel("upscale_preset", values["upscale_preset"]), "Upscaling"),
            ("02", "Frame generation", FrameGenerationSummary(values).Detail, FrameGenerationSummary(values).Group),
            ("03", "Screen & video", values["resolution"] + "  ·  " + values["vblank"] + " Hz", "Display") };
        for (var i = 0; i < quickData.Length; i++)
        {
            var (number, title, value, group) = quickData[i];
            var stack = new StackPanel { Spacing = 7 };
            stack.Children.Add(Text(number + "  /  " + title.ToUpperInvariant(), 14, "#A5B6CA"));
            stack.Children.Add(Text(value, 23));
            var card = Button("", () => ShowSettings(Selected, group)); card.Content = stack; card.Background = Brush("#B3182130");
            quick.Children.Add(card); Grid.SetColumn(card, i); _actions.Add(card);
        }
        Grid.SetRow(quick, 2); page.Children.Add(quick); _body.Content = page;
        if (_games.Count > 0) Focus(_tiles[_selected]); else Focus(add);
        UpdateHints();
    }

    private Button MakeTile(Game game, double size, bool label, Action action)
    {
        var image = new Image { Stretch = Stretch.UniformToFill, Width = size - 14, Height = size - 14 };
        if (game.Icon is { } path)
        {
            if (!_icons.TryGetValue(path, out var bitmap))
            {
                try { using var stream = File.OpenRead(path); bitmap = Bitmap.DecodeToWidth(stream, 256); _icons[path] = bitmap; }
                catch (Exception e) when (e is IOException or ArgumentException or NotSupportedException) { }
            }
            image.Source = bitmap;
        }
        var art = new Grid { Width = size - 14, Height = size - 14, ClipToBounds = true };
        art.Children.Add(Text(game.Name, 24)); art.Children.Add(image);
        var content = new StackPanel { Spacing = 12 }; content.Children.Add(art);
        if (label) content.Children.Add(new TextBlock { Text = game.Name, FontSize = 19, TextTrimming = TextTrimming.CharacterEllipsis, MaxWidth = size - 14 });
        var button = Button("", action, "tile"); button.Content = content; button.Width = size;
        return button;
    }

    private void ShowLibrary()
    {
        _page = "library"; _tiles.Clear(); _actions.Clear(); _backdrop.Opacity = .2;
        for (var i = 0; i < _tabs.Count; i++) _tabs[i].Classes.Set("active", i == 1);
        var page = new Grid { RowDefinitions = new("90,*"), Margin = new(68, 10, 68, 0) };
        var heading = new Grid { ColumnDefinitions = new("*,Auto") };
        heading.Children.Add(Text($"Game library   /   {_games.Count}", 37, weight: FontWeight.Light));
        var add = Button("+  Add games", () => BrowseFolder(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), AddFolder));
        Grid.SetColumn(add, 1); heading.Children.Add(add); page.Children.Add(heading); _actions.Add(add);
        var grid = new Avalonia.Controls.Primitives.UniformGrid { Columns = 6, VerticalAlignment = VerticalAlignment.Top };
        for (var i = 0; i < _games.Count; i++)
        {
            var index = i;
            var tile = MakeTile(_games[i], 216, true, () => { _selected = index; OpenPage("home"); _ = UpdateArtwork(); });
            tile.Height = 270; tile.Margin = new(0, 0, 18, 20); _tiles.Add(tile); grid.Children.Add(tile);
            tile.GotFocus += (_, _) =>
            {
                if (_selected == index) return;
                _selected = index; _preferences.LastGame = Selected?.Path ?? ""; _ = UpdateArtwork();
            };
        }
        var scroll = new ScrollViewer { Content = grid }; Grid.SetRow(scroll, 1); page.Children.Add(scroll);
        _body.Content = page; Focus(_tiles.ElementAtOrDefault(_selected) ?? add); UpdateHints();
    }

    private void SelectGame(int index, bool rebuild = true)
    {
        _selected = Math.Clamp(index, 0, Math.Max(0, _games.Count - 1));
        _preferences.LastGame = Selected?.Path ?? "";
        if (rebuild) ShowHome();
        else Dispatcher.UIThread.Post(() => ShowHome());
        _ = UpdateArtwork();
    }

    private async Task UpdateArtwork()
    {
        var generation = ++_artGeneration;
        var path = Selected?.Background;
        var bitmap = await Task.Run(() =>
        {
            try { if (path is null) return null; using var stream = File.OpenRead(path); return Bitmap.DecodeToWidth(stream, 1600); }
            catch (Exception e) when (e is IOException or ArgumentException or NotSupportedException) { return null; }
        });
        if (generation != _artGeneration) { bitmap?.Dispose(); return; }
        _backdrop.Source = bitmap;
        _backgroundBitmap?.Dispose(); _backgroundBitmap = bitmap;
        PlayMusic();
    }

    private async void AddFolder(string folder)
    {
        if (!_preferences.GameFolders.Contains(folder)) _preferences.GameFolders.Add(folder);
        _preferences.ExcludedGames.RemoveAll(path => path == folder || path.StartsWith(folder + System.IO.Path.DirectorySeparatorChar, StringComparison.Ordinal));
        _preferences.Save(); await ReloadLibrary();
    }

    private void UpdateHints()
    {
        var pad = _pad.Connected;
        _hints.Text = _modal is not null
            ? (pad ? "D-pad  Navigate       ✕  Select       ○  Back" : "Arrows  Navigate       Enter  Select       Esc  Back")
            : _page == "settings"
                ? (pad ? "D-pad  Navigate       ◀ ▶  Change       ✕  Select       ○  Back       L1 / R1  Category" : "Arrows  Navigate / change       Enter  Select       Esc  Back       Q / E  Category")
                : (pad ? "✕  Play / select       △  Game settings       □  Library       L1 / R1  Page       PS  Control centre"
                    : "Enter  Play / select       F3  Game settings       F4  Library       Q / E  Page       F10  Control centre");
    }

    private void OnInputKey(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.F11) { WindowState = WindowState == WindowState.FullScreen ? WindowState.Maximized : WindowState.FullScreen; e.Handled = true; return; }
        var action = e.Key switch { Key.Up => PadAction.Up, Key.Down => PadAction.Down, Key.Left => PadAction.Left, Key.Right => PadAction.Right,
            Key.Enter => PadAction.Accept, Key.Escape => PadAction.Back, Key.F3 => PadAction.Settings,
            Key.F4 => PadAction.Library, Key.Q => PadAction.Previous, Key.E => PadAction.Next, Key.F10 => PadAction.Menu, _ => (PadAction?)null };
        if (_editor is not null && FocusManager?.GetFocusedElement() == _editor && e.Key != Key.Escape) return;
        if (action is { } input) { Navigate(input); e.Handled = true; }
    }

    private void Navigate(PadAction action)
    {
        using var soundOperation = _audio.Sounds.Operation();
        if (_launching) return;
        if (action == PadAction.Back) _audio.Sounds.Request(UiSound.Back);
        if (action is PadAction.Previous or PadAction.Next && _editor is null)
            _audio.Sounds.Request(action == PadAction.Previous ? UiSound.Previous : UiSound.Next);
        if (_modal is not null) { NavigateModal(action); return; }
        if (_gameProcess is not null && action is not PadAction.Menu) return;
        if (action == PadAction.Menu) { ShowSystem(); return; }
        if (action == PadAction.Settings && Selected is not null) { ShowSettings(Selected); return; }
        if (action == PadAction.Library) { OpenPage("library"); return; }
        if (_page == "settings") { NavigateSettings(action); return; }
        var focus = FocusManager?.GetFocusedElement() as Button;
        var tab = _tabs.IndexOf(focus!);
        if (action is PadAction.Previous or PadAction.Next)
        {
            var index = _page == "home" ? 0 : 1;
            OpenPage(new[] { "home", "library", "settings" }[Math.Clamp(index + (action == PadAction.Next ? 1 : -1), 0, 2)]); return;
        }
        if (action == PadAction.Back) { OpenPage("home"); return; }
        if (tab >= 0)
        {
            if (action == PadAction.Left || action == PadAction.Right) Focus(_tabs[Math.Clamp(tab + (action == PadAction.Right ? 1 : -1), 0, 2)]);
            else if (action == PadAction.Down) Focus(_tiles.ElementAtOrDefault(_selected) ?? _actions.FirstOrDefault());
            else if (action == PadAction.Accept) focus!.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            return;
        }
        var tile = _tiles.IndexOf(focus!);
        if (tile >= 0)
        {
            var step = _page == "library" ? 6 : 1;
            if (action is PadAction.Left or PadAction.Right || _page == "library" && action is PadAction.Up or PadAction.Down)
            {
                var delta = action == PadAction.Left ? -1 : action == PadAction.Right ? 1 : action == PadAction.Up ? -step : step;
                var next = tile + delta;
                if (next < 0 && action == PadAction.Up) Focus(_actions.FirstOrDefault());
                else Focus(_tiles[Math.Clamp(next, 0, _tiles.Count - 1)]);
            }
            else if (action == PadAction.Up) Focus(_tabs[0]);
            else if (action == PadAction.Down) Focus(_actions.FirstOrDefault());
            else if (action == PadAction.Accept)
            {
                if (tile < _games.Count) { _selected = tile; Launch(); }
                else focus!.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            }
            return;
        }
        var item = _actions.IndexOf(focus!);
        if (action == PadAction.Accept) focus?.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
        else if (action == PadAction.Up) Focus(item >= 2 ? _actions[0] : _tiles.ElementAtOrDefault(_selected) ?? _tabs[0]);
        else if (action == PadAction.Down) Focus(_actions.ElementAtOrDefault(_page == "home" ? 2 : 0));
        else if (action is PadAction.Left or PadAction.Right && _actions.Count > 0) Focus(_actions[Math.Clamp(item + (action == PadAction.Right ? 1 : -1), 0, _actions.Count - 1)]);
    }

    private async void Launch()
    {
        if (Selected is not { } game || _gameProcess is not null || _launching) return;
        _launching = true;
        try
        {
            var command = KytyBridge.Build(game.Path, game.TitleId);
            _audio.Sounds.Request(UiSound.Launch);
            await _audio.SuspendForGameAsync();
            if (_closing) return;
            var info = new ProcessStartInfo(command.Exe) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var argument in command.Arguments) info.ArgumentList.Add(argument);
            foreach (var (key, value) in command.Environment) info.Environment[key] = value;
            // Match the existing isolated frontend's runtime environment for the validated selector.
            var user = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            info.Environment["XDG_CONFIG_HOME"] = System.IO.Path.Combine(user, ".config/kyty-sharp-frontend/xdg");
            info.Environment["XDG_DATA_HOME"] = System.IO.Path.Combine(user, ".local/share/kyty-sharp-frontend");
            var logDir = System.IO.Path.Combine(Preferences.Root, "logs"); Directory.CreateDirectory(logDir);
            _logFile = System.IO.Path.Combine(logDir, $"{game.TitleId}-{DateTime.Now:yyyyMMdd-HHmmss}.log");
            _logLines.Clear();
            var process = new Process { StartInfo = info };
            _gameProcess = process;
            // Stream to disk without accumulating a session-sized log in memory.
            using var writer = new StreamWriter(_logFile, false) { AutoFlush = true };
            void Log(object sender, DataReceivedEventArgs args)
            {
                if (args.Data is null) return;
                lock (writer) { writer.WriteLine(args.Data); _logLines.Enqueue(args.Data); while (_logLines.Count > 150) _logLines.Dequeue(); }
            }
            process.OutputDataReceived += Log; process.ErrorDataReceived += Log;
            if (!process.Start()) throw new IOException("Could not start the native runtime.");
            _launching = false;
            process.BeginOutputReadLine(); process.BeginErrorReadLine();
            _inputTimer.Interval = TimeSpan.FromMilliseconds(250); ShowHome();
            await process.WaitForExitAsync();
            process.WaitForExit();
            var code = process.ExitCode;
            process.OutputDataReceived -= Log; process.ErrorDataReceived -= Log; process.Dispose(); _gameProcess = null;
            _inputTimer.Interval = TimeSpan.FromMilliseconds(33);
            if (_closing) return;
            _audio.Resume(); OpenPage("home"); Activate(); PlayMusic();
            if (code != 0) Message("Game session ended", $"{game.Name} exited with code {code}.\nSession log: {_logFile}");
        }
        catch (Exception e)
        {
            _gameProcess?.Dispose(); _gameProcess = null; _inputTimer.Interval = TimeSpan.FromMilliseconds(33);
            _audio.Resume(); PlayMusic(); _audio.Sounds.Request(UiSound.Error);
            Message("Could not launch game", e.Message);
        }
        finally { _launching = false; }
    }

    private void StopGame() { try { _gameProcess?.Kill(true); } catch (InvalidOperationException) { } }
    private void PlayMusic()
    {
        if (_gameProcess is not null || _launching || _closing) return;
        var source = MusicLibrary.Resolve(_preferences.Audio);
        var preview = _preferences.PlayTitleMusic && Selected is { } game ? System.IO.Path.Combine(game.Folder, "sce_sys", "snd0.at9") : null;
        var usePreview = preview is not null && File.Exists(preview);
        _audio.Configure(_preferences.Audio, usePreview ? _preferences.MusicVolume / 100f : 1);
        if (_preferences.Audio.MusicEnabled) _ = _audio.SelectMusicAsync(usePreview ? preview : source);
    }
}

using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Media;

namespace KytyPS5;

internal sealed partial class ConsoleWindow
{
    private TextBlock? _audioStatus;
    private TextBlock? _musicCredit;
    private UiSound _testSound = UiSound.Confirm;
    private IDisposable? _audioSaveTimer;
    private void RefreshAudioStatus()
    {
        if (_page != "settings" || _groups[_category].Id != "Audio" || _audioStatus is null) return;
        var status = _audio.Status;
        if (_audioStatus.Text != status) _audioStatus.Text = status;
    }
    private void SaveAudio(bool persistImmediately = true)
    {
        _preferences.Audio.Normalize(); PlayMusic();
        _audioSaveTimer?.Dispose();
        if (persistImmediately) _preferences.Save();
        else _audioSaveTimer = Avalonia.Threading.DispatcherTimer.RunOnce(() => _preferences.Save(), TimeSpan.FromMilliseconds(150));
        _saveStatus.Text = "✓  Audio settings saved  /  Applied immediately";
    }
    private void AddAudioSettings(StackPanel content)
    {
        var options = _preferences.Audio;
        content.Children.Add(Note("Dashboard audio is shared by all games. Music and interface sounds have independent volume and mute controls."));
        void Toggle(string title, Func<bool> get, Action<bool> set)
        {
            var row = Setting(title, get() ? "On" : "Off");
            void Change(int direction)
            {
                using var operation = _audio.Sounds.Operation();
                set(!get()); SaveAudio(); UpdateRowValue(row, get() ? "On" : "Off");
                _audio.Sounds.Request(get() ? UiSound.Enable : UiSound.Disable);
            }
            row.Click += (_, _) => Change(1); _adjust[row] = Change; content.Children.Add(row);
        }
        Toggle("Background music", () => options.MusicEnabled, enabled => options.MusicEnabled = enabled);
        var theme = Setting("Music theme", MusicLibrary.Themes.First(t => t.Id == options.Theme).Title, "All four requested compositions, plus your custom music.");
        theme.Click += (_, _) => ShowMusicThemes();
        _adjust[theme] = direction =>
        {
            var index = Array.FindIndex(MusicLibrary.Themes, t => t.Id == options.Theme);
            SelectMusicTheme(MusicLibrary.Themes[(index + direction + MusicLibrary.Themes.Length) % MusicLibrary.Themes.Length].Id);
            UpdateRowValue(theme, MusicLibrary.Themes.First(t => t.Id == options.Theme).Title);
        };
        content.Children.Add(theme);
        AddVolumeSlider(content, "Music volume", () => options.MusicVolume, value => options.MusicVolume = value);
        Toggle("Interface sounds", () => options.SoundsEnabled, enabled => options.SoundsEnabled = enabled);
        AddVolumeSlider(content, "Interface sounds volume", () => options.SoundsVolume, value => options.SoundsVolume = value);
        var test = Setting("Test sound", SoundLabel(_testSound), "Select to preview. Left / right chooses another effect, including distinct L1 and R1 sounds.");
        test.Classes.Add("audioPreview");
        test.Click += (_, _) => _audio.Sounds.Preview(_testSound);
        _adjust[test] = direction =>
        {
            var sounds = Enum.GetValues<UiSound>(); _testSound = sounds[((int)_testSound + direction + sounds.Length) % sounds.Length];
            UpdateRowValue(test, SoundLabel(_testSound)); _audio.Sounds.Preview(_testSound);
        };
        content.Children.Add(test);
        var import = Setting("Import local music", "Choose file", "Import for the selected theme. MP3, OGG, FLAC, WAV, M4A, AAC and Opus are supported.");
        import.Click += (_, _) => BrowseFolder(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ImportMusic, files: true);
        content.Children.Add(import);
        _musicCredit = Text("", 16, "#A7B9CE"); content.Children.Add(_musicCredit); UpdateMusicCredit();
        var credits = Setting("Music sources & credits", "View", "Links to the artists' free downloads on OverClocked ReMix.");
        credits.Click += (_, _) => ShowMusicCredits(); content.Children.Add(credits);
        _audioStatus = Text(_audio.Status, 15, "#A7B9CE"); content.Children.Add(_audioStatus);
        _saveStatus.Text = "Audio changes take effect immediately.";
    }
    private void AddVolumeSlider(StackPanel content, string title, Func<int> read, Action<int> write)
    {
        var row = Setting(title, read() + "%");
        var grid = new Grid { ColumnDefinitions = new("*,440,90"), ColumnSpacing = 22 };
        grid.Children.Add(Text(title, 22));
        var slider = new Slider { Minimum = 0, Maximum = 100, Value = read(), SmallChange = 1, LargeChange = 5,
            TickFrequency = 1, IsSnapToTickEnabled = true, Focusable = false, VerticalAlignment = VerticalAlignment.Center };
        Avalonia.Automation.AutomationProperties.SetName(slider, title);
        slider.AddHandler(Avalonia.Input.InputElement.PointerPressedEvent, (_, _) => Focus(row),
            Avalonia.Interactivity.RoutingStrategies.Tunnel, handledEventsToo: true);
        var value = Text(read() + "%", 22, "#BBDFFF"); value.TextAlignment = TextAlignment.Right;
        Grid.SetColumn(slider, 1); Grid.SetColumn(value, 2); grid.Children.Add(slider); grid.Children.Add(value); row.Content = grid;
        slider.ValueChanged += (_, _) =>
        {
            var next = (int)Math.Round(slider.Value);
            if (next == read()) return;
            write(next); value.Text = next + "%"; SaveAudio(persistImmediately: false);
        };
        _adjust[row] = direction => { slider.Value = Math.Clamp(slider.Value + direction * 5, 0, 100); _audio.Sounds.Request(UiSound.Move); };
        row.Click += (_, _) => { slider.Value = Math.Clamp(slider.Value + 5, 0, 100); };
        content.Children.Add(row);
    }
    private void SelectMusicTheme(string id)
    {
        _preferences.Audio.Theme = id;
        _preferences.PlayTitleMusic = false; // Selecting a dashboard theme takes precedence over optional game previews.
        SaveAudio(); UpdateMusicCredit(); _audio.Sounds.Request(MusicLibrary.Resolve(_preferences.Audio) is null ? UiSound.Error : UiSound.Confirm);
    }
    private void UpdateMusicCredit()
    {
        if (_musicCredit is null) return;
        var theme = MusicLibrary.Themes.First(t => t.Id == _preferences.Audio.Theme);
        var file = MusicLibrary.Resolve(_preferences.Audio);
        _musicCredit.Text = file is null ? theme.Composition + "\nMissing audio file — choose Import local music."
            : _preferences.Audio.Imports.ContainsKey(theme.Id) ? theme.Composition + "\nImported local copy: " + System.IO.Path.GetFileName(file)
            : theme.Composition + "\n“" + theme.Arrangement + "” — " + theme.Artist + ", OverClocked ReMix (https://ocremix.org)\n" + theme.Source;
    }
    private void ShowMusicThemes()
    {
        var stack = BeginModal("Music themes", "Choose a composition. Every entry supports importing a local copy.");
        foreach (var theme in MusicLibrary.Themes)
        {
            var available = MusicLibrary.Resolve(_preferences.Audio, theme.Id) is not null;
            var button = Button("", () => { SelectMusicTheme(theme.Id); CloseModal(); SetCategory(_category); Focus(_rows.ElementAtOrDefault(1)); });
            button.Content = new StackPanel { Spacing = 5, Children = {
                Text(theme.Composition, 23), Text(available ? (theme.Id == "custom" || _preferences.Audio.Imports.ContainsKey(theme.Id) ? "Local copy ready" : theme.Arrangement + "  /  " + theme.Artist + "  /  OverClocked ReMix (https://ocremix.org)")
                    : "Not installed  /  Select, then use Import local music", 15, available ? "#AAC6E3" : "#F3C48C") } };
            stack.Children.Add(button); _modalRows.Add([button]);
        }
        ModalButtons(stack, ("Back", CloseModal));
        var index = Array.FindIndex(MusicLibrary.Themes, t => t.Id == _preferences.Audio.Theme);
        Focus(_modalRows[Math.Max(0, index)][0]);
    }
    private async void ImportMusic(string file)
    {
        var id = _preferences.Audio.Theme;
        try
        {
            _saveStatus.Text = "Importing and checking audio…";
            var destination = await MusicLibrary.ImportAsync(file, id);
            _preferences.Audio.Imports[id] = destination; _preferences.PlayTitleMusic = false;
            SaveAudio(); if (_page == "settings" && _groups[_category].Id == "Audio") { SetCategory(_category); Focus(_rows.ElementAtOrDefault(6)); }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or System.ComponentModel.Win32Exception or OperationCanceledException)
        {
            Message("Could not import music", error.Message);
        }
    }
    private void ShowMusicCredits()
    {
        var stack = BeginModal("Music sources & credits", "Free fan arrangements for personal listening. Select a track to open its official source.");
        foreach (var theme in MusicLibrary.Themes.Where(t => t.Id != "custom"))
        {
            var button = Button("", () => StartTool("/usr/bin/xdg-open", theme.Source));
            button.Content = new StackPanel { Spacing = 6, Children = {
                Text(theme.Composition, 22), Text(theme.Arrangement + " — " + theme.Artist + ", OverClocked ReMix (https://ocremix.org)\n" + theme.Source, 16, "#A7B9CE") } };
            stack.Children.Add(button); _modalRows.Add([button]);
        }
        ModalButtons(stack, ("Back", CloseModal)); Focus(_modalRows[0][0]);
    }
    private static string SoundLabel(UiSound sound) => sound switch
    {
        UiSound.Move => "Move", UiSound.Confirm => "Confirm", UiSound.Back => "Back / cancel",
        UiSound.Previous => "L1 / previous", UiSound.Next => "R1 / next", UiSound.Open => "Open panel",
        UiSound.Close => "Close panel", UiSound.Enable => "Enable setting", UiSound.Disable => "Disable setting",
        UiSound.Error => "Error / unavailable", UiSound.Launch => "Launch game", _ => sound.ToString(),
    };
}

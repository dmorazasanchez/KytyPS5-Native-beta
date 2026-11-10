using System.Diagnostics;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;

namespace KytyPS5;

internal sealed partial class ConsoleWindow
{
    private Border? _modal;
    private readonly List<List<Button>> _modalRows = [];
    private Control? _returnFocus;
    private TextBox? _editor;
    private Action? _modalBack;

    private StackPanel BeginModal(string title, string description)
    {
        _audio.Sounds.Request(UiSound.Open);
        if (_modal is null) _returnFocus = FocusManager?.GetFocusedElement() as Control;
        else _scene.Children.Remove(_modal);
        _modalRows.Clear(); _editor = null;
        var stack = new StackPanel { Spacing = 18 };
        stack.Children.Add(Text(title, 39, weight: FontWeight.Light));
        if (!string.IsNullOrEmpty(description)) stack.Children.Add(Text(description, 19, "#AEBFD3"));
        var layout = new Grid { RowDefinitions = new("*,Auto") };
        layout.Children.Add(stack);
        var hints = Text(_pad.Connected ? "D-pad  Navigate      ✕  Select      ○  Back" : "Arrows  Navigate      Enter  Select      Esc  Back", 17, "#AEBFD3");
        Grid.SetRow(hints, 1); layout.Children.Add(hints);
        _modal = new Border { Background = Brush("#0B111C"), Padding = new(90, 42), Child = layout };
        _scene.Children.Add(_modal); _modalBack = CloseModal; UpdateHints(); return stack;
    }

    private void CloseModal()
    {
        if (_modal is not null) _audio.Sounds.Request(UiSound.Close);
        if (_modal is not null) _scene.Children.Remove(_modal);
        _modal = null; _editor = null; _modalBack = null; _modalRows.Clear(); UpdateHints();
        Focus(_returnFocus ?? (_page == "settings" ? _categories.ElementAtOrDefault(_category) : _tiles.ElementAtOrDefault(_selected)));
    }

    private void ModalButtons(StackPanel stack, params (string Label, Action Action)[] actions)
    {
        var buttons = new List<Button>();
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        foreach (var (label, action) in actions)
        {
            var button = Button(label, action); buttons.Add(button); row.Children.Add(button);
        }
        _modalRows.Add(buttons); stack.Children.Add(row);
    }

    private void Message(string title, string description)
    {
        _audio.Sounds.Request(UiSound.Error);
        var stack = BeginModal(title, description); ModalButtons(stack, ("OK", CloseModal)); Focus(_modalRows[0][0]);
    }

    private void Confirm(string title, string description, string confirm, Action action)
    {
        var stack = BeginModal(title, description);
        ModalButtons(stack, ("Cancel", CloseModal), (confirm, () => { CloseModal(); action(); })); Focus(_modalRows[0][0]);
    }

    private void EditText(string title, string value, Action<string> save, bool multiline = false, bool browse = false)
    {
        var stack = BeginModal(title, "Use the on-screen keys or select the text field to type. R1: Save. L1: Delete. Shift: capitals and symbols.");
        stack.Spacing = 12;
        var editor = new TextBox { Text = value, AcceptsReturn = multiline, Height = multiline ? 152 : 68,
            TextWrapping = TextWrapping.Wrap, CaretIndex = value.Length };
        _editor = editor; stack.Children.Add(editor);
        void Insert(string text)
        {
            var original = editor.Text ?? "";
            var start = Math.Clamp(Math.Min(editor.SelectionStart, editor.SelectionEnd), 0, original.Length);
            var end = Math.Clamp(Math.Max(editor.SelectionStart, editor.SelectionEnd), start, original.Length);
            if (start == end) start = end = Math.Clamp(editor.CaretIndex, 0, original.Length);
            editor.Text = original[..start] + text + original[end..];
            editor.CaretIndex = start + text.Length; editor.SelectionStart = editor.SelectionEnd = editor.CaretIndex;
        }
        void Delete()
        {
            var original = editor.Text ?? ""; var index = Math.Clamp(editor.CaretIndex, 0, original.Length);
            if (index > 0) { editor.Text = original.Remove(index - 1, 1); editor.CaretIndex = index - 1; editor.SelectionStart = editor.SelectionEnd = editor.CaretIndex; }
        }
        var keys = new StackPanel { Spacing = 9, Margin = new(0, 8) };
        var shifted = false;
        var letters = new List<(Button Button, char Character)>();
        char Shift(char character)
        {
            return character switch {
                '1' => '!', '2' => '@', '3' => '#', '4' => '$', '5' => '%', '6' => '^', '7' => '&',
                '8' => '*', '9' => '(', '0' => ')', '-' => '_', '=' => '+', '[' => '{', ']' => '}',
                ';' => ':', ':' => '`', '\'' => '"', ',' => '<', '.' => '>', '/' => '?', '\\' => '|', '#' => '~',
                _ => char.ToUpperInvariant(character) };
        }
        foreach (var line in new[] { "1234567890-_=", "qwertyuiop[]", "asdfghjkl;:'", "zxcvbnm,./\\#" })
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 9 };
            var buttons = new List<Button>();
            foreach (var character in line)
            {
                var key = Button(character.ToString(), () => Insert((shifted ? Shift(character) : character).ToString()), "key");
                key.Width = 86; row.Children.Add(key); buttons.Add(key); letters.Add((key, character));
            }
            keys.Children.Add(row); _modalRows.Add(buttons);
        }
        stack.Children.Add(keys);
        var tools = new StackPanel();
        var actions = new List<(string, Action)> {
            ("Shift", () => { shifted = !shifted; foreach (var (key, character) in letters) key.Content = (shifted ? Shift(character) : character).ToString(); }),
            ("Space", () => Insert(" ")), ("⌫  Delete", Delete), ("@", () => Insert("@")), ("$", () => Insert("$")),
            ("%", () => Insert("%")), ("+", () => Insert("+")),
            ("←", () => { editor.CaretIndex = Math.Max(0, editor.CaretIndex - 1); editor.SelectionStart = editor.SelectionEnd = editor.CaretIndex; }),
            ("→", () => { editor.CaretIndex = Math.Min(editor.Text?.Length ?? 0, editor.CaretIndex + 1); editor.SelectionStart = editor.SelectionEnd = editor.CaretIndex; }),
            ("Clear", () => { editor.Text = ""; editor.CaretIndex = 0; }) };
        if (multiline) actions.Add(("New line", () => Insert("\n")));
        ModalButtons(tools, actions.ToArray()); stack.Children.Add(tools);
        var footer = new List<(string, Action)> { ("Cancel", CloseModal), ("Save", () => { var text = editor.Text ?? ""; CloseModal(); save(text); }) };
        if (browse) footer.Insert(1, ("Browse…", () =>
        {
            var current = editor.Text ?? "";
            var initial = Directory.Exists(current) ? current : System.IO.Path.GetDirectoryName(current);
            BrowseFolder(Directory.Exists(initial) ? initial! : Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                path => EditText(title, path, save, multiline, browse), true,
                () => EditText(title, current, save, multiline, browse));
        }));
        ModalButtons(stack, footer.ToArray());
        _deleteCharacter = Delete;
        Focus(_modalRows[0][0]);
    }
    private Action? _deleteCharacter;

    private void BrowseFolder(string folder, Action<string> selected, bool files = false, Action? back = null, int offset = 0)
    {
        var stack = BeginModal(files ? "Choose a file or folder" : "Add games", folder);
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        ModalButtons(stack,
            ("←  Parent folder", () => BrowseFolder(Directory.GetParent(folder)?.FullName ?? folder, selected, files, back)),
            ("Home", () => BrowseFolder(home, selected, files, back)),
            ("Use this folder", () => { CloseModal(); selected(folder); }),
            ("Cancel", () => { if (back is not null) back(); else CloseModal(); }));
        var entries = new StackPanel { Spacing = 8 };
        try
        {
            var directories = Directory.EnumerateDirectories(folder).Where(p => !System.IO.Path.GetFileName(p).StartsWith('.'))
                .OrderBy(p => p, StringComparer.OrdinalIgnoreCase);
            var paths = directories.Concat(Directory.EnumerateFiles(folder)
                .Where(p => files || GameArchive.IsGameFile(p)).OrderBy(p => p, StringComparer.OrdinalIgnoreCase));
            var page = paths.Skip(offset).Take(151).ToList();
            foreach (var path in page.Take(150))
            {
                var directory = Directory.Exists(path);
                var row = Button((directory ? "▸   " : "     ") + System.IO.Path.GetFileName(path), () =>
                {
                    if (directory) BrowseFolder(path, selected, files, back);
                    else { CloseModal(); selected(path); }
                });
                row.MinHeight = 55; entries.Children.Add(row); _modalRows.Add([row]);
            }
            if (page.Count == 0) entries.Children.Add(Text("No matching files or folders here. You can choose this folder or go back.", 19, "#A7B9CE"));
            if (page.Count > 150)
            {
                var more = Button("Next page  →", () => BrowseFolder(folder, selected, files, back, offset + 150));
                entries.Children.Add(more); _modalRows.Add([more]);
            }
            if (offset > 0)
            {
                var previous = Button("←  Previous page", () => BrowseFolder(folder, selected, files, back, Math.Max(0, offset - 150)));
                entries.Children.Add(previous); _modalRows.Add([previous]);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { entries.Children.Add(Text(e.Message)); }
        stack.Children.Add(new ScrollViewer { Content = entries, Height = 535 });
        _modalBack = () => { if (back is not null) back(); else CloseModal(); };
        Focus(_modalRows.ElementAtOrDefault(1)?.FirstOrDefault() ?? _modalRows[0][0]);
    }

    private void NavigateModal(PadAction action)
    {
        if (action == PadAction.Back) { _modalBack?.Invoke(); return; }
        if (action == PadAction.Menu) { CloseModal(); return; }
        if (_editor is not null && action == PadAction.Previous) { _deleteCharacter?.Invoke(); return; }
        if (_editor is not null && action == PadAction.Next) { Focus(_modalRows[^1].Last()); return; }
        var focused = FocusManager?.GetFocusedElement() as Button;
        var y = _modalRows.FindIndex(row => row.Contains(focused!));
        if (y < 0) { Focus(_modalRows.FirstOrDefault()?.FirstOrDefault()); return; }
        var x = _modalRows[y].IndexOf(focused!);
        if (action == PadAction.Accept) { focused?.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent)); return; }
        if (action is PadAction.Left or PadAction.Right)
            Focus(_modalRows[y][Math.Clamp(x + (action == PadAction.Right ? 1 : -1), 0, _modalRows[y].Count - 1)]);
        else if (action is PadAction.Up or PadAction.Down)
        {
            var next = Math.Clamp(y + (action == PadAction.Down ? 1 : -1), 0, _modalRows.Count - 1);
            Focus(_modalRows[next][Math.Min(x, _modalRows[next].Count - 1)]);
        }
    }

    private void ShowSystem()
    {
        var stack = BeginModal("Control centre", _gameProcess is null ? "KytyPS5  /  Ready to play" : "A game session is running.");
        var items = new List<(string, Action)> { ("Back", CloseModal), ("Global settings", () => { CloseModal(); ShowSettings(null); }),
            ("Session log", ShowLog), ("Open terminal", () => { CloseModal(); StartTool("/usr/bin/konsole"); }) };
        if (_gameProcess is not null) items.Add(("Stop game", () => Confirm("Stop game?", "Unsaved game progress may be lost.", "Stop game", () => StopGame())));
        foreach (var item in items) ModalButtons(stack, item);
        if (File.Exists((Environment.GetEnvironmentVariable("KYTYPS5_SESSION_CONTROL") ?? "/var/lib/kyty-beta-system/session/ps5-minimal-control")))
            ModalButtons(stack, ("Return to Desktop Mode", () => Confirm("Return to Desktop Mode?", "Save your game first. The PS5 session will close and KDE will open.", "Switch to desktop", () => StartTool("/usr/bin/pkexec", (Environment.GetEnvironmentVariable("KYTYPS5_SESSION_CONTROL") ?? "/var/lib/kyty-beta-system/session/ps5-minimal-control"), "desktop"))));
        ModalButtons(stack, ("Exit KytyPS5", () => Confirm("Exit KytyPS5?", _gameProcess is null ? "Close the launcher?" : "The running game will be stopped.", "Exit", () => { StopGame(); _closing = true; Close(); })));
        Focus(_modalRows[0][0]);
    }

    private void ShowLog()
    {
        var stack = BeginModal("Session log", _logFile ?? "No game session has run yet.");
        var text = File.Exists(_logFile) ? string.Join('\n', File.ReadLines(_logFile!).TakeLast(150)) : "No log available.";
        var log = new TextBox { Text = text, IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap,
            FontFamily = FontFamily.Parse("monospace"), FontSize = 16, Height = 570 };
        stack.Children.Add(log);
        ModalButtons(stack, ("Back", ShowSystem), ("Scroll up", () => log.CaretIndex = Math.Max(0, log.CaretIndex - 800)),
            ("Scroll down", () => log.CaretIndex = Math.Min(log.Text?.Length ?? 0, log.CaretIndex + 800)));
        log.CaretIndex = text.Length; Focus(_modalRows[0][0]);
    }

    private async void StartTool(string tool, params string[] arguments)
    {
        try
        {
            var info = new ProcessStartInfo(tool) { UseShellExecute = false };
            foreach (var argument in arguments) info.ArgumentList.Add(argument);
            using var process = Process.Start(info);
            if (process is not null) { await process.WaitForExitAsync(); if (process.ExitCode != 0) Message("Action unavailable", $"{System.IO.Path.GetFileName(tool)} exited with code {process.ExitCode}."); }
        }
        catch (Exception e) { Message("Action unavailable", e.Message); }
    }
}

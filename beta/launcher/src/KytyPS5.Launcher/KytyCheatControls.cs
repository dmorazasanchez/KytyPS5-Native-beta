// BC-250 Kyty: per-game JSON cheat selection.
// Changes are staged in the game patch file for the next emulator launch.
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace KytyPS5;

internal sealed class KytyCheatControls
{
    private readonly string? _planPath;
    private readonly byte[]? _originalBytes;
    private readonly List<(CheckBox Box, bool Original)> _options = new();
    public TabItem Tab { get; }

    public KytyCheatControls(string? titleId, string? gamePath, string? configuredPath)
    {
        var stack = new StackPanel { Spacing = 16, Margin = new Thickness(26) };
        stack.Children.Add(new TextBlock {
            Text = "Per-game cheats", FontSize = 26
        });
        stack.Children.Add(new TextBlock {
            Text = "Choose cheats for the NEXT launch. PS / F10 opens the same choices in-game. " +
                   "Multi-byte code hooks require a full game restart; active instructions are never rewritten.",
            TextWrapping = TextWrapping.Wrap, FontSize = 19
        });

        if (string.IsNullOrWhiteSpace(titleId) || string.IsNullOrWhiteSpace(gamePath))
        {
            stack.Children.Add(Message("Select a game to manage its cheats."));
        }
        else if (string.IsNullOrWhiteSpace(configuredPath) || !File.Exists(configuredPath))
        {
            stack.Children.Add(Message("No cheats JSON is configured for this game. " +
                "Set its JSON file path under Patches / Advanced, save, then reopen this window."));
        }
        else if (!KytyBridge.PatchMatchesGame(configuredPath, gamePath, titleId))
        {
            stack.Children.Add(Message("The configured cheat JSON does not match this game's " +
                "title ID or version. No cheats will be changed."));
        }
        else
        {
            try
            {
                var bytes = File.ReadAllBytes(configuredPath);
                using var doc = JsonDocument.Parse(WithoutBom(bytes));
                var mods = doc.RootElement.GetProperty("mods");
                if (mods.ValueKind != JsonValueKind.Array || mods.GetArrayLength() == 0)
                    throw new InvalidDataException("No cheats in this plan.");

                foreach (var mod in mods.EnumerateArray())
                {
                    var name = mod.GetProperty("name").GetString();
                    if (string.IsNullOrWhiteSpace(name))
                        throw new InvalidDataException("Cheat has no name.");
                    var enabled = !mod.TryGetProperty("enabled", out var value) ||
                                  value.ValueKind == JsonValueKind.True;
                    if (mod.TryGetProperty("enabled", out value) &&
                        value.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                        throw new InvalidDataException("Invalid cheat enabled state.");
                    var toggle = new CheckBox {
                        Content = name, IsChecked = enabled, FontSize = 22,
                        MinHeight = 48
                    };
                    stack.Children.Add(toggle);
                    _options.Add((toggle, enabled));
                }
                _planPath = configuredPath;
                _originalBytes = bytes;
                var quickActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16 };
                var enableAll = new Button { Content = "Enable all", MinHeight = 48 };
                var disableAll = new Button { Content = "Disable all", MinHeight = 48 };
                enableAll.Click += (_, _) => { foreach (var (box, _) in _options) box.IsChecked = true; };
                disableAll.Click += (_, _) => { foreach (var (box, _) in _options) box.IsChecked = false; };
                quickActions.Children.Add(enableAll);
                quickActions.Children.Add(disableAll);
                stack.Children.Add(quickActions);
                stack.Children.Add(new TextBlock {
                    Text = configuredPath, TextWrapping = TextWrapping.Wrap, FontSize = 14
                });
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or
                                       JsonException or InvalidDataException or KeyNotFoundException)
            {
                _options.Clear();
                stack.Children.Add(Message("Could not load cheat choices: " + ex.Message));
            }
        }
        Tab = new TabItem { Header = "Cheats", Content = new ScrollViewer { Content = stack } };
    }

    private static TextBlock Message(string text) => new() {
        Text = text, FontSize = 19, TextWrapping = TextWrapping.Wrap
    };

    private static ReadOnlyMemory<byte> WithoutBom(byte[] bytes) =>
        bytes.AsSpan().StartsWith(new byte[] { 0xEF, 0xBB, 0xBF }) ? bytes.AsMemory(3) : bytes;

    public bool Save(out string error)
    {
        error = "";
        if (_planPath is null || _originalBytes is null || _options.Count == 0)
            return true;
        var selections = _options.Select(item => item.Box.IsChecked == true).ToArray();
        if (!selections.Where((value, index) => value != _options[index].Original).Any())
            return true;

        try
        {
            var current = File.ReadAllBytes(_planPath);
            // An in-game PS-menu edit might have happened since this window opened.
            if (!_originalBytes.SequenceEqual(current))
            {
                error = "Cheat file changed while this window was open. Close and reopen settings.";
                return false;
            }
            var root = JsonNode.Parse(WithoutBom(current).Span) as JsonObject ??
                       throw new InvalidDataException("Invalid cheat document.");
            var mods = root["mods"] as JsonArray ??
                       throw new InvalidDataException("Missing cheat list.");
            if (mods.Count != _options.Count)
                throw new InvalidDataException("Cheat list changed.");
            for (var i = 0; i < selections.Length; i++)
            {
                var mod = mods[i] as JsonObject ?? throw new InvalidDataException("Bad cheat entry.");
                if (mod["name"]?.GetValue<string>() != _options[i].Box.Content?.ToString())
                    throw new InvalidDataException("Cheat order changed.");
                mod["enabled"] = selections[i];
            }
            var temp = _planPath + ".kytyps5-cheats-" + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllText(temp, root.ToJsonString(new JsonSerializerOptions {
                    WriteIndented = true
                }) + "\n", new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
                File.Move(temp, _planPath, overwrite: true);
            }
            finally
            {
                if (File.Exists(temp)) File.Delete(temp);
            }
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or
                                   JsonException or InvalidDataException or InvalidOperationException or
                                   ArgumentException)
        {
            error = "Unable to save cheats: " + ex.Message;
            return false;
        }
    }
}

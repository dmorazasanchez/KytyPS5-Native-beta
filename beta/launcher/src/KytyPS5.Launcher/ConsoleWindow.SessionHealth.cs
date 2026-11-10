using System.Text.Json;

namespace KytyPS5;

internal sealed partial class ConsoleWindow
{
    private readonly string? _sessionHealthPath = Environment.GetEnvironmentVariable("KYTYPS5_SESSION_HEALTH");
    private long _sessionHealthAt;
    private int _sessionTicks;
    private bool _libraryLoaded;

    private void PublishSessionHealth()
    {
        if (_sessionHealthPath is null || Environment.TickCount64 < _sessionHealthAt) return;
        _sessionHealthAt = Environment.TickCount64 + 500;
        try
        {
            var temp = _sessionHealthPath + ".tmp";
            File.WriteAllText(temp, JsonSerializer.Serialize(new {
                pid = Environment.ProcessId, ticks = ++_sessionTicks,
                time = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), active = IsActive,
                visible = IsVisible, loaded = _libraryLoaded, controller = _pad.Connected,
                controllerName = _pad.Name, width = Bounds.Width, height = Bounds.Height
            }));
            File.Move(temp, _sessionHealthPath, true);
        }
        catch (IOException error) { Console.Error.WriteLine("[session health] " + error.Message); }
    }
}

using System.Runtime.InteropServices;

namespace KytyPS5;

internal enum PadAction { Up, Down, Left, Right, Accept, Back, Settings, Library, Previous, Next, Menu }

/// <summary>SDL's mapped controller API supports DualSense, Xbox and standard pads. No render loop.</summary>
internal sealed class Gamepad : IDisposable
{
    private const string Sdl = "libSDL2-2.0.so.0";
    private IntPtr _pad;
    private readonly bool _ready;
    private readonly int? _preferredIndex;
    private uint _previous;
    private long _scanAt;
    private readonly Dictionary<PadAction, long> _repeats = [];
    public bool Connected { get; private set; }
    public string Name { get; private set; } = "Controller";
    public Gamepad(int? preferredIndex = null)
    {
        _preferredIndex = preferredIndex;
        try
        {
            SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
            SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "1");
            SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
            _ready = SDL_InitSubSystem(0x2200) == 0;
        }
        catch (DllNotFoundException) { }
    }

    public List<PadAction> Poll(bool active)
    {
        var result = new List<PadAction>();
        if (!_ready) return result;
        SDL_PumpEvents();
        SDL_GameControllerUpdate();
        SDL_FlushEvents(0, 0xFFFF);
        if (_pad != IntPtr.Zero && SDL_GameControllerGetAttached(_pad) == 0)
        {
            SDL_GameControllerClose(_pad); _pad = IntPtr.Zero; _previous = 0; _repeats.Clear();
        }
        var now = Environment.TickCount64;
        if (_pad == IntPtr.Zero && now >= _scanAt)
        {
            _scanAt = now + 1000;
            for (var i = 0; i < SDL_NumJoysticks(); i++)
            {
                if (_preferredIndex is { } preferred && i != preferred) continue;
                if (SDL_IsGameController(i) == 0) continue;
                _pad = SDL_GameControllerOpen(i);
                if (_pad != IntPtr.Zero) { Name = Marshal.PtrToStringUTF8(SDL_GameControllerName(_pad)) ?? "Controller"; break; }
            }
        }
        Connected = _pad != IntPtr.Zero;
        if (!Connected) return result;
        uint buttons = 0;
        for (var i = 0; i <= 14; i++) if (SDL_GameControllerGetButton(_pad, i) != 0) buttons |= 1u << i;
        var x = SDL_GameControllerGetAxis(_pad, 0);
        var y = SDL_GameControllerGetAxis(_pad, 1);
        if (x < -16000) buttons |= 1u << 13;
        if (x > 16000) buttons |= 1u << 14;
        if (y < -16000) buttons |= 1u << 11;
        if (y > 16000) buttons |= 1u << 12;
        var mapping = new (int, PadAction)[] { (11, PadAction.Up), (12, PadAction.Down),
            (13, PadAction.Left), (14, PadAction.Right), (0, PadAction.Accept), (1, PadAction.Back),
            (2, PadAction.Library), (3, PadAction.Settings), (9, PadAction.Previous), (10, PadAction.Next),
            (5, PadAction.Menu), (6, PadAction.Menu) };
        foreach (var (bit, action) in mapping)
        {
            var held = (buttons & (1u << bit)) != 0;
            if (!active || !held) { _repeats.Remove(action); continue; }
            if ((_previous & (1u << bit)) == 0)
            {
                result.Add(action); _repeats[action] = now + 380;
            }
            else if (action is PadAction.Up or PadAction.Down or PadAction.Left or PadAction.Right &&
                _repeats.TryGetValue(action, out var at) && now >= at)
            {
                result.Add(action); _repeats[action] = now + 110;
            }
        }
        _previous = buttons;
        return result;
    }

    public void Dispose()
    {
        if (_pad != IntPtr.Zero) SDL_GameControllerClose(_pad);
        if (_ready) SDL_QuitSubSystem(0x2200);
    }
    [DllImport(Sdl)] private static extern int SDL_InitSubSystem(uint flags);
    [DllImport(Sdl)] private static extern void SDL_QuitSubSystem(uint flags);
    [DllImport(Sdl)] private static extern int SDL_SetHint(string name, string value);
    [DllImport(Sdl)] private static extern void SDL_PumpEvents();
    [DllImport(Sdl)] private static extern void SDL_GameControllerUpdate();
    [DllImport(Sdl)] private static extern void SDL_FlushEvents(uint minimum, uint maximum);
    [DllImport(Sdl)] private static extern int SDL_NumJoysticks();
    [DllImport(Sdl)] private static extern int SDL_IsGameController(int index);
    [DllImport(Sdl)] private static extern IntPtr SDL_GameControllerOpen(int index);
    [DllImport(Sdl)] private static extern void SDL_GameControllerClose(IntPtr pad);
    [DllImport(Sdl)] private static extern int SDL_GameControllerGetAttached(IntPtr pad);
    [DllImport(Sdl)] private static extern byte SDL_GameControllerGetButton(IntPtr pad, int button);
    [DllImport(Sdl)] private static extern short SDL_GameControllerGetAxis(IntPtr pad, int axis);
    [DllImport(Sdl)] private static extern IntPtr SDL_GameControllerName(IntPtr pad);
}

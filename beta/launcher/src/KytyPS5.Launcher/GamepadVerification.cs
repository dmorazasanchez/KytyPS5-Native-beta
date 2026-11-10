using System.Runtime.InteropServices;

namespace KytyPS5;

internal static class GamepadVerification
{
    private const string Sdl = "libSDL2-2.0.so.0";
    public static void Run(Action<bool, string> check)
    {
        if (SDL_InitSubSystem(0x2200) != 0) throw new InvalidOperationException("SDL could not initialize for controller verification.");
        var index = SDL_JoystickAttachVirtual(1, 6, 15, 0);
        if (index < 0) throw new InvalidOperationException("SDL could not attach a virtual controller.");
        var joystick = SDL_JoystickOpen(index);
        try
        {
            using var pad = new Gamepad(index);
            pad.Poll(true); check(pad.Connected, "SDL controller discovery");
            foreach (var (button, action) in new[] { (0, PadAction.Accept), (1, PadAction.Back), (2, PadAction.Library),
                (3, PadAction.Settings), (5, PadAction.Menu), (6, PadAction.Menu), (9, PadAction.Previous),
                (10, PadAction.Next), (11, PadAction.Up), (12, PadAction.Down), (13, PadAction.Left), (14, PadAction.Right) })
            {
                SDL_JoystickSetVirtualButton(joystick, button, 1);
                check(pad.Poll(true).Contains(action), "SDL mapped button: " + button + " → " + action);
                check(!pad.Poll(true).Contains(action), "SDL edge detection: " + action);
                SDL_JoystickSetVirtualButton(joystick, button, 0); pad.Poll(true);
            }
            SDL_JoystickSetVirtualAxis(joystick, 0, 22000); check(pad.Poll(true).Contains(PadAction.Right), "Analog stick navigation");
            SDL_JoystickSetVirtualAxis(joystick, 0, 0); pad.Poll(true);
            SDL_JoystickSetVirtualButton(joystick, 0, 1); check(pad.Poll(false).Count == 0, "Background input suppression");
            check(pad.Poll(true).Count == 0, "Held game input is not replayed on refocus");
            SDL_JoystickSetVirtualButton(joystick, 0, 0); pad.Poll(true);
            SDL_JoystickDetachVirtual(index); pad.Poll(true);
            check(!pad.Connected, "Controller disconnection");
        }
        finally
        {
            SDL_JoystickClose(joystick);
            if (SDL_JoystickIsVirtual(index) != 0) SDL_JoystickDetachVirtual(index);
            SDL_QuitSubSystem(0x2200);
        }
    }
    [DllImport(Sdl)] private static extern int SDL_InitSubSystem(uint flags);
    [DllImport(Sdl)] private static extern void SDL_QuitSubSystem(uint flags);
    [DllImport(Sdl)] private static extern int SDL_JoystickAttachVirtual(int type, int axes, int buttons, int hats);
    [DllImport(Sdl)] private static extern int SDL_JoystickDetachVirtual(int index);
    [DllImport(Sdl)] private static extern int SDL_JoystickIsVirtual(int index);
    [DllImport(Sdl)] private static extern IntPtr SDL_JoystickOpen(int index);
    [DllImport(Sdl)] private static extern void SDL_JoystickClose(IntPtr joystick);
    [DllImport(Sdl)] private static extern int SDL_JoystickSetVirtualButton(IntPtr joystick, int button, byte value);
    [DllImport(Sdl)] private static extern int SDL_JoystickSetVirtualAxis(IntPtr joystick, int axis, short value);
}

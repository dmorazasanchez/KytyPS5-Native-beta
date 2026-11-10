using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Fonts.Inter;
using Avalonia.Markup.Xaml;
using Avalonia.Themes.Fluent;

namespace KytyPS5;

internal static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--verify")) return Verification.Run();
        if (args.Contains("--list-games"))
        {
            Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(Game.Scan(Preferences.Load()),
                new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        if (args.Contains("--audio-output-check")) return AudioVerification.OutputProbe().GetAwaiter().GetResult();
        if (args.Contains("--audio-device-probe"))
        {
            using var engine = new AudioEngine(); Thread.Sleep(150); Console.WriteLine(engine.Status); return 0;
        }
        if (args.Contains("--prepare-music"))
        {
            var options = Preferences.Load().Audio;
            foreach (var theme in MusicLibrary.Themes.Where(t => t.Id != "custom"))
            {
                var file = MusicLibrary.Resolve(options, theme.Id);
                if (file is null) { Console.WriteLine(theme.Composition + ": MISSING — import a local copy"); continue; }
                Console.WriteLine(theme.Composition + ": " + MusicLibrary.PrepareAsync(file).GetAwaiter().GetResult());
            }
            return 0;
        }
        if (args.Length == 2 && args[0] == "--inspect-launch")
        {
            var game = Game.Read(args[1]);
            Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(KytyBridge.Build(game.Path, game.TitleId),
                new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        Directory.CreateDirectory(Preferences.Root);
        FileStream instance;
        try { instance = new FileStream(System.IO.Path.Combine(Preferences.Root, "launcher.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException) { return 0; }
        using (instance)
        {
            try
            {
                AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().StartWithClassicDesktopLifetime(args);
                return Environment.ExitCode;
            }
            catch (Exception error)
            {
                File.AppendAllText(System.IO.Path.Combine(Preferences.Root, "gui-crash.log"), $"{DateTime.Now:O} {error}\n");
                Console.Error.WriteLine(error); return 1;
            }
        }
    }
}

public sealed class App : Application
{
    public override void Initialize()
    {
        RequestedThemeVariant = Avalonia.Styling.ThemeVariant.Dark;
        Styles.Add(new FluentTheme());
        Styles.Add((Avalonia.Styling.Styles)AvaloniaXamlLoader.Load(new Uri("avares://KytyPS5/Styles.axaml")));
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new ConsoleWindow();
        base.OnFrameworkInitializationCompleted();
    }
}

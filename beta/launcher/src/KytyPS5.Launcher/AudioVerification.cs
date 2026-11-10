using System.Reflection;
using System.Text.Json;
using System.Runtime.InteropServices;

namespace KytyPS5;

internal static class AudioVerification
{
    public static async Task<int> OutputProbe()
    {
        using var engine = new AudioEngine();
        var options = new AudioPreferences { MusicVolume = 25, SoundsVolume = 50 };
        void Stage(string name) => Console.WriteLine(JsonSerializer.Serialize(new { stage = name, utcMilliseconds = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), status = engine.Status, voices = engine.MusicVoices }));
        engine.Configure(options); await engine.SelectMusicAsync(MusicLibrary.Resolve(options));
        await Task.Delay(100); Stage("music"); await Task.Delay(1500);
        options.Theme = "dearly"; await engine.SelectMusicAsync(MusicLibrary.Resolve(options)); Stage("crossfade"); await Task.Delay(1300);
        options.MusicEnabled = false; options.SoundsVolume = 65; engine.Configure(options); await Task.Delay(100);
        Stage("effects_only"); engine.Play(UiSound.Confirm); await Task.Delay(700);
        options.SoundsVolume = 0; engine.Configure(options); await Task.Delay(100); Stage("muted"); await Task.Delay(600);
        await engine.SuspendForGameAsync(); Stage("game_silence"); await Task.Delay(700);
        options.MusicEnabled = true; engine.Configure(options); engine.Resume(); Stage("resume"); await Task.Delay(1000);
        Stage("complete"); return engine.Status.StartsWith("Audio output:") ? 0 : 1;
    }
    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "kyty-audio-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(root);
        var previousRoot = Environment.GetEnvironmentVariable("KYTYPS5_AUDIO_ROOT");
        var previousDriver = Environment.GetEnvironmentVariable("SDL_AUDIODRIVER");
        Environment.SetEnvironmentVariable("KYTYPS5_AUDIO_ROOT", root);
        try
        {
            check(MusicLibrary.Themes.Length == 5 && MusicLibrary.Themes.Count(t => t.Source.StartsWith("https://ocremix.org/")) == 4,
                "Four exact compositions and a fifth custom theme");
            var preferences = new AudioPreferences();
            check(preferences.MusicEnabled && preferences.SoundsEnabled && preferences.MusicVolume == 25 && preferences.SoundsVolume == 50 && preferences.Theme == "fairy", "Recommended audio defaults");
            var restored = JsonSerializer.Deserialize<Preferences>(JsonSerializer.Serialize(new Preferences {
                Audio = new AudioPreferences { MusicVolume = 15, SoundsVolume = 65, MusicEnabled = false, SoundsEnabled = true, Theme = "dearly" },
                AdditionalProperties = new() { ["futureLauncherOption"] = JsonDocument.Parse("42").RootElement.Clone() } }))!;
            check(restored.Audio.MusicVolume == 15 && restored.Audio.SoundsVolume == 65 && !restored.Audio.MusicEnabled && restored.Audio.SoundsEnabled && restored.AdditionalProperties!.ContainsKey("futureLauncherOption"), "Independent preferences round-trip and unknown launcher fields survive");
            preferences.MusicVolume = -1; preferences.SoundsVolume = 900; preferences.Normalize();
            check(preferences.MusicVolume == 0 && preferences.SoundsVolume == 100, "Volume range normalization");
            foreach (var sound in Enum.GetValues<UiSound>())
            {
                using var resource = Assembly.GetExecutingAssembly().GetManifestResourceStream("KytyPS5.Sound." + sound.ToString().ToLowerInvariant());
                check(resource is not null && resource.Length > 0 && resource.Length % 8 == 0, "Preloaded CC0 effect: " + sound);
            }
            using (var left = Assembly.GetExecutingAssembly().GetManifestResourceStream("KytyPS5.Sound.previous")!)
            using (var right = Assembly.GetExecutingAssembly().GetManifestResourceStream("KytyPS5.Sound.next")!)
                check(left.Length != right.Length, "L1 and R1 use distinguishable pitch and stereo direction");
            var pcm = Path.Combine(root, "constant.s16"); WritePcm(pcm, 4, _ => .08f);
            float Sample(float musicGain, float soundGain, bool effect)
            {
                using var mixer = new AudioMixer(); mixer.SetVolumes(musicGain, soundGain); mixer.ChangeMusic(new PcmLoop(pcm));
                mixer.Render(new float[96000]);
                if (effect) mixer.EffectSound(Enumerable.Repeat(.02f, 96000).ToArray());
                var output = new float[1024]; mixer.Render(output); return output.Average();
            }
            var music = Sample(.15f, 0, false); var sounds = Sample(0, .65f, true); var both = Sample(.15f, .65f, true);
            check(Math.Abs(both - music - sounds) < .00002, "15% music and 65% effects mix independently");
            check(Sample(0, 0, true) == 0, "Both 0% volume controls produce exact digital silence");
            check(sounds > .01 && Sample(0, .65f, false) == 0, "Music mute does not disable effects");
            check(music > .01 && Math.Abs(Sample(.15f, 0, true) - music) < .00002, "Effects mute does not disable music");
            check(Math.Abs(Sample(.30f, 0, false) - music * 2) < .00002, "Live music gain scales independently");
            using (var mixer = new AudioMixer())
            {
                mixer.ChangeMusic(new PcmLoop(pcm)); mixer.Render(new float[96000]);
                mixer.ChangeMusic(new PcmLoop(pcm)); check(mixer.MusicVoices == 2, "Theme crossfade overlaps exactly two active voices");
                mixer.Render(new float[96000]); check(mixer.MusicVoices == 1, "Outgoing crossfade voice is released");
                for (var n = 0; n < 20; n++) mixer.ChangeMusic(new PcmLoop(pcm));
                check(mixer.MusicVoices <= 3, "Rapid theme changes stay bounded");
                mixer.SetMaster(0, 450); var output = new float[48000]; mixer.Render(output);
                check(output.TakeLast(1024).All(sample => sample == 0), "Launch fade reaches exact silence before handoff");
                mixer.SetMaster(1, 650); mixer.Render(new float[96000]); mixer.Render(output);
                check(output.Any(sample => sample != 0), "Return-to-GUI fade resumes music");
            }
            var seam = Path.Combine(root, "seam.s16"); WritePcm(seam, .237, i => (float)(.2 * Math.Sin(i * 2 * Math.PI * 440 / 48000)));
            using (var mixer = new AudioMixer())
            {
                mixer.SetVolumes(1, 0); mixer.ChangeMusic(new PcmLoop(seam)); var output = new float[48000 * 2 * 3]; mixer.Render(output);
                var maxJump = Enumerable.Range(1, output.Length / 2 - 1).Max(i => Math.Abs(output[i * 2] - output[(i - 1) * 2]));
                check(maxJump < .04, "Repeated loop seams avoid abrupt sample discontinuities");
            }
            var played = new List<UiSound>(); var router = new UiSoundRouter(played.Add);
            using (router.Operation()) { router.Request(UiSound.Move); using (router.Operation()) router.Request(UiSound.Confirm); router.Request(UiSound.Open); }
            check(played.SequenceEqual(new[] { UiSound.Open }), "Nested mouse/key/controller events emit one semantic sound");
            played.Clear(); using (router.Operation()) { router.Request(UiSound.Confirm); router.Request(UiSound.Enable); }
            check(played.SequenceEqual(new[] { UiSound.Enable }), "Toggle sound replaces duplicate confirm feedback");
            var wave = Path.Combine(root, "import.wav"); WriteWave(wave);
            var imported = MusicLibrary.ImportAsync(wave, "custom").GetAwaiter().GetResult();
            preferences = new AudioPreferences { Imports = new() { ["custom"] = imported }, Theme = "custom" };
            check(File.Exists(imported) && MusicLibrary.Resolve(preferences) == imported, "Custom music import validates, copies and resolves a real local file");
            preferences.Theme = "fairy"; check(MusicLibrary.Resolve(preferences) is null, "Missing required themes retain their entries for import");
            Environment.SetEnvironmentVariable("SDL_AUDIODRIVER", "dummy");
            SDL_SetHint("SDL_AUDIODRIVER", "dummy");
            using (var entered = new ManualResetEventSlim())
            using (var release = new ManualResetEventSlim())
            using (var engine = new AudioEngine(() => { entered.Set(); release.Wait(); }))
            {
                try
                {
                    check(entered.Wait(TimeSpan.FromSeconds(3)), "Audio backend stall regression reaches device initialization");
                    var ui = Task.Run(() => { engine.Configure(new AudioPreferences()); engine.Play(UiSound.Confirm); _ = engine.Status; });
                    check(ui.Wait(TimeSpan.FromMilliseconds(500)), "A stalled native audio backend cannot block GUI configuration, sounds or status");
                }
                finally { release.Set(); }
            }
            using (var engine = new AudioEngine())
            {
                engine.Configure(new AudioPreferences()); engine.SelectMusicAsync(wave).GetAwaiter().GetResult();
                engine.SelectMusicAsync(wave).GetAwaiter().GetResult();
                check(engine.MusicVoices == 1, "Repeated requests never duplicate the selected playback voice");
                engine.Play(UiSound.Confirm);
                for (var attempt = 0; attempt < 100 && engine.Status.Contains("Starting audio"); attempt++) Thread.Sleep(20);
                check(engine.Status.Contains("dummy"), "SDL audio backend runs with a bounded playback queue: " + engine.Status);
                engine.SuspendForGameAsync().GetAwaiter().GetResult(); check(engine.Suspended, "Game handoff suspends and releases the SDL output device");
                engine.Resume(); Thread.Sleep(100); check(!engine.Suspended, "SDL device reopens on return to the dashboard");
            }
        }
        finally
        {
            Environment.SetEnvironmentVariable("KYTYPS5_AUDIO_ROOT", previousRoot);
            Environment.SetEnvironmentVariable("SDL_AUDIODRIVER", previousDriver); Directory.Delete(root, true);
            SDL_ResetHint("SDL_AUDIODRIVER");
        }
    }
    [DllImport("libSDL2-2.0.so.0")] private static extern int SDL_SetHint(string name, string value);
    [DllImport("libSDL2-2.0.so.0")] private static extern int SDL_ResetHint(string name);
    private static void WritePcm(string path, double seconds, Func<int, float> sample)
    {
        using var file = new BinaryWriter(File.Create(path));
        for (var i = 0; i < (int)(seconds * 48000); i++) { var value = (short)(sample(i) * 32767); file.Write(value); file.Write(value); }
    }
    private static void WriteWave(string path)
    {
        var frames = 48000;
        using var file = new BinaryWriter(File.Create(path));
        file.Write("RIFF"u8); file.Write(36 + frames * 4); file.Write("WAVEfmt "u8); file.Write(16);
        file.Write((short)1); file.Write((short)2); file.Write(48000); file.Write(192000); file.Write((short)4); file.Write((short)16);
        file.Write("data"u8); file.Write(frames * 4);
        for (var i = 0; i < frames; i++) { var sample = (short)(Math.Sin(i * 2 * Math.PI * 440 / 48000) * 5000); file.Write(sample); file.Write(sample); }
    }
}

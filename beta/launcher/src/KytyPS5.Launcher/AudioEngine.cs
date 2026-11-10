using System.Reflection;
using System.Runtime.InteropServices;

namespace KytyPS5;

internal sealed class AudioEngine : IDisposable
{
    private const string Sdl = "libSDL2-2.0.so.0";
    private readonly object _sync = new();
    private readonly AudioMixer _mixer = new();
    private readonly Dictionary<UiSound, float[]> _sounds = [];
    private readonly CancellationTokenSource _stop = new();
    private readonly Task _pump;
    private readonly Action? _beforeDeviceOpen;
    private readonly float[] _buffer = new float[1024];
    private readonly byte[] _bytes = new byte[4096];
    private uint _device;
    private bool _initialized, _suspended, _disposed, _reopenRequested;
    private TaskCompletionSource? _streamReleased;
    private int _generation;
    private string? _selected, _prepared;
    private CancellationTokenSource? _selectionStop;
    private long _retryAt;
    private volatile string _deviceStatus = "Starting audio…";
    private string _musicStatus = "";
    public string Status { get { lock (_sync) return _deviceStatus + (_musicStatus.Length > 0 ? "  /  " + _musicStatus : ""); } }
    public int MusicVoices { get { lock (_sync) return _mixer.MusicVoices; } }
    public bool Suspended { get { lock (_sync) return _suspended; } }
    public UiSoundRouter Sounds { get; }

    public AudioEngine(Action? beforeDeviceOpen = null)
    {
        _beforeDeviceOpen = beforeDeviceOpen;
        foreach (var sound in Enum.GetValues<UiSound>())
        {
            using var resource = Assembly.GetExecutingAssembly().GetManifestResourceStream("KytyPS5.Sound." + sound.ToString().ToLowerInvariant());
            if (resource is null) continue;
            using var bytes = new MemoryStream(); resource.CopyTo(bytes);
            var pcm = new float[bytes.Length / 4]; Buffer.BlockCopy(bytes.ToArray(), 0, pcm, 0, pcm.Length * 4); _sounds[sound] = pcm;
        }
        Sounds = new UiSoundRouter(Play);
        _pump = Task.Run(Pump);
    }

    public void Configure(AudioPreferences options, float previewGain = 1)
    {
        lock (_sync)
        {
            _mixer.SetVolumes(options.MusicEnabled ? options.MusicVolume / 100f * previewGain : 0,
                options.SoundsEnabled ? options.SoundsVolume / 100f : 0);
            if (!options.SoundsEnabled || options.SoundsVolume == 0) _mixer.ClearEffects();
        }
    }

    public async Task SelectMusicAsync(string? source)
    {
        int generation;
        CancellationTokenSource selection;
        lock (_sync)
        {
            if (_disposed || _selected == source && (_prepared is not null || _selectionStop is not null || source is null)) return;
            _selectionStop?.Cancel();
            selection = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token); _selectionStop = selection;
            generation = ++_generation; _selected = source; _prepared = null;
            if (source is null)
            {
                _mixer.StopMusic(); _musicStatus = "Music file missing — use Import local copy";
                _selectionStop = null; selection.Dispose(); return;
            }
            _musicStatus = "Preparing music…";
        }
        try
        {
            var prepared = await MusicLibrary.PrepareAsync(source, selection.Token).ConfigureAwait(false);
            PcmLoop? next = new PcmLoop(prepared);
            try
            {
                lock (_sync)
                {
                    if (_disposed || generation != _generation) return;
                    _prepared = prepared;
                    _mixer.ChangeMusic(next); next = null; _musicStatus = "Music ready";
                }
            }
            finally { next?.Dispose(); }
        }
        catch (OperationCanceledException) { }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or System.ComponentModel.Win32Exception)
        {
            lock (_sync)
            {
                if (_disposed || generation != _generation) return;
                _selected = null; _prepared = null; _mixer.StopMusic(); _musicStatus = "Music unavailable: " + error.Message;
            }
        }
        finally
        {
            lock (_sync) { if (ReferenceEquals(_selectionStop, selection)) _selectionStop = null; }
            selection.Dispose();
        }
    }

    public void Play(UiSound sound)
    {
        lock (_sync) { if (!_disposed && !_suspended && _sounds.TryGetValue(sound, out var samples)) _mixer.EffectSound(samples); }
    }

    public async Task SuspendForGameAsync()
    {
        lock (_sync) { if (_disposed) return; _mixer.SetMaster(0, 450); }
        await Task.Delay(510).ConfigureAwait(false);
        Task released;
        lock (_sync)
        {
            _suspended = true; _mixer.ClearEffects();
            _streamReleased = new(TaskCreationOptions.RunContinuationsAsynchronously);
            released = _streamReleased.Task;
        }
        // Native device operations belong to the audio worker. An unavailable
        // audio server must never hold the UI/mixer lock or prevent launching.
        try { await released.WaitAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false); }
        catch (TimeoutException) { Console.Error.WriteLine("[KytyPS5 audio] Playback release timed out; continuing without waiting for the audio backend."); }
        await Task.Delay(40).ConfigureAwait(false);
    }

    public void Resume()
    {
        lock (_sync) { if (_disposed) return; _suspended = false; _reopenRequested = true; _mixer.SetMaster(1, 650); }
    }

    private void OpenDevice()
    {
        if (_device != 0 || Environment.TickCount64 < _retryAt) return;
        _retryAt = Environment.TickCount64 + 5000;
        try
        {
            _beforeDeviceOpen?.Invoke();
            if (_stop.IsCancellationRequested) return;
            if (!_initialized)
            {
                SDL_SetHint("SDL_AUDIO_DEVICE_APP_NAME", "KytyPS5");
                SDL_SetHint("SDL_AUDIO_DEVICE_STREAM_NAME", "Dashboard music and interface");
                if (SDL_InitSubSystem(0x10) != 0) { _deviceStatus = "Audio device unavailable (retrying)"; return; }
                _initialized = true;
            }
            var desired = new AudioSpec { Frequency = 48000, Format = 0x8120, Channels = 2, Samples = 512 };
            _device = SDL_OpenAudioDevice(IntPtr.Zero, 0, ref desired, out _, 0);
            if (_device == 0) { _deviceStatus = "Audio device unavailable (retrying)"; return; }
            SDL_PauseAudioDevice(_device, 0);
            _deviceStatus = "Audio output: " + (Marshal.PtrToStringUTF8(SDL_GetCurrentAudioDriver()) ?? "system default");
        }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException)
        {
            _deviceStatus = "Audio backend unavailable: " + error.Message; _retryAt = long.MaxValue;
        }
    }

    private async Task Pump()
    {
        try
        {
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(8));
            while (await timer.WaitForNextTickAsync(_stop.Token).ConfigureAwait(false))
            {
                bool suspended, disposed;
                lock (_sync)
                {
                    suspended = _suspended; disposed = _disposed;
                    if (_reopenRequested) { _retryAt = 0; _reopenRequested = false; }
                }
                if (disposed) break;
                if (suspended)
                {
                    CloseDevice();
                    lock (_sync) _streamReleased?.TrySetResult();
                    continue;
                }
                // No SDL/server call is made while holding _sync: even a hung
                // native backend cannot block GUI construction or input sounds.
                OpenDevice();
                if (_device == 0) continue;
                if (SDL_GetAudioDeviceStatus(_device) == 0) { CloseDevice(); _retryAt = 0; continue; }
                bool audible;
                lock (_sync) audible = _mixer.HasAudibleWork;
                if (!audible && SDL_GetQueuedAudioSize(_device) == 0)
                {
                    SDL_PauseAudioDevice(_device, 1); continue;
                }
                SDL_PauseAudioDevice(_device, 0);
                // Bound each fill pass even if a backend reports a draining queue.
                for (var fill = 0; fill < 16 && SDL_GetQueuedAudioSize(_device) < 12288; fill++)
                {
                    lock (_sync)
                    {
                        if (_suspended || _disposed) break;
                        _mixer.Render(_buffer);
                    }
                    Buffer.BlockCopy(_buffer, 0, _bytes, 0, _bytes.Length);
                    if (SDL_QueueAudio(_device, _bytes, (uint)_bytes.Length) != 0) { CloseDevice(); break; }
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception error)
        {
            _deviceStatus = "Audio paused: " + error.Message;
            Console.Error.WriteLine("[KytyPS5 audio] " + error.Message);
        }
        finally
        {
            CloseDevice();
            if (_initialized) SDL_QuitSubSystem(0x10);
            lock (_sync) { _mixer.Dispose(); _streamReleased?.TrySetResult(); }
        }
    }
    private void CloseDevice()
    {
        if (_device == 0) return;
        SDL_PauseAudioDevice(_device, 1); SDL_ClearQueuedAudio(_device); SDL_CloseAudioDevice(_device); _device = 0;
    }
    public void Dispose()
    {
        lock (_sync) { if (_disposed) return; _disposed = true; }
        _stop.Cancel();
        if (_pump.Wait(TimeSpan.FromSeconds(2))) _stop.Dispose();
        else
        {
            Console.Error.WriteLine("[KytyPS5 audio] Audio backend did not stop; leaving cleanup to the worker.");
            _ = _pump.ContinueWith(_ => _stop.Dispose(), TaskScheduler.Default);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct AudioSpec
    {
        public int Frequency; public ushort Format; public byte Channels, Silence;
        public ushort Samples, Padding; public uint Size; public IntPtr Callback, UserData;
    }
    [DllImport(Sdl)] private static extern int SDL_InitSubSystem(uint flags);
    [DllImport(Sdl)] private static extern void SDL_QuitSubSystem(uint flags);
    [DllImport(Sdl)] private static extern int SDL_SetHint(string name, string value);
    [DllImport(Sdl)] private static extern uint SDL_OpenAudioDevice(IntPtr name, int capture, ref AudioSpec desired, out AudioSpec obtained, int changes);
    [DllImport(Sdl)] private static extern void SDL_CloseAudioDevice(uint device);
    [DllImport(Sdl)] private static extern void SDL_PauseAudioDevice(uint device, int pause);
    [DllImport(Sdl)] private static extern int SDL_GetAudioDeviceStatus(uint device);
    [DllImport(Sdl)] private static extern uint SDL_GetQueuedAudioSize(uint device);
    [DllImport(Sdl)] private static extern int SDL_QueueAudio(uint device, byte[] data, uint length);
    [DllImport(Sdl)] private static extern void SDL_ClearQueuedAudio(uint device);
    [DllImport(Sdl)] private static extern IntPtr SDL_GetCurrentAudioDriver();
}

/// <summary>Nested input events form one operation. The strongest semantic sound wins over focus/click noise.</summary>
internal sealed class UiSoundRouter(Action<UiSound> play)
{
    private int _depth;
    private UiSound? _pending;
    private long _lastMove, _quietFocusUntil;
    private bool _flushing;
    private bool _scheduled;
    public Action<Action> Schedule { get; set; } = callback => callback();
    public int Emitted { get; private set; }
    public IDisposable Operation() { _depth++; return new Scope(this); }
    private sealed class Scope(UiSoundRouter owner) : IDisposable { public void Dispose() { if (--owner._depth == 0) owner.QueueFlush(); } }
    private static int Priority(UiSound sound) => sound switch
    {
        UiSound.Launch or UiSound.Error => 100, UiSound.Previous or UiSound.Next => 90,
        UiSound.Enable or UiSound.Disable => 85, UiSound.Back => 80,
        UiSound.Open or UiSound.Close => 70, UiSound.Confirm => 50, _ => 10,
    };
    public void Request(UiSound sound)
    {
        var now = Environment.TickCount64;
        if (sound == UiSound.Move && (now < _quietFocusUntil || now - _lastMove < 55)) return;
        if (_pending is null || Priority(sound) > Priority(_pending.Value)) _pending = sound;
        if (_depth == 0) QueueFlush();
    }
    public void Preview(UiSound sound) { _pending = sound; if (_depth == 0) QueueFlush(); }
    private void QueueFlush()
    {
        if (_scheduled || _pending is null) return;
        _scheduled = true; Schedule(() => { _scheduled = false; Flush(); });
    }
    private void Flush()
    {
        if (_pending is not { } sound || _flushing) return;
        _pending = null; _flushing = true;
        try
        {
            if (sound == UiSound.Move) _lastMove = Environment.TickCount64;
            else _quietFocusUntil = Environment.TickCount64 + 110;
            play(sound); Emitted++;
        }
        finally { _flushing = false; }
    }
}

using System.Buffers.Binary;

namespace KytyPS5;

internal enum UiSound { Move, Confirm, Back, Previous, Next, Open, Close, Enable, Disable, Error, Launch }

/// <summary>Buffered PCM reader with a short overlap at the loop seam; memory is bounded by two small buffers.</summary>
internal sealed class PcmLoop : IDisposable
{
    private readonly FileStream _file;
    private readonly byte[] _buffer = new byte[32768];
    private readonly float[] _head;
    private readonly long _start;
    private readonly int _overlap;
    private long _position;
    private int _offset, _count;
    public long Frames { get; }
    public long Position => _position;
    public PcmLoop(string path)
    {
        _file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 65536, FileOptions.SequentialScan);
        try
        {
            if (_file.Length < 1920 || _file.Length % 4 != 0) throw new InvalidDataException("Invalid decoded stereo music.");
            // Remove only silence at the outer edges, preserving pauses within the composition.
            var first = FindEdge(false); var last = FindEdge(true);
            _start = Math.Max(0, first - 240);
            Frames = Math.Min(_file.Length / 4, last + 241) - _start;
            if (Frames < 480) throw new InvalidDataException("The music file is silent or too short.");
            _overlap = (int)Math.Min(57600, Frames / 4);
            _head = new float[_overlap * 2]; _file.Position = _start * 4;
            var bytes = new byte[_overlap * 4]; _file.ReadExactly(bytes);
            for (var i = 0; i < _head.Length; i++) _head[i] = BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(i * 2)) / 32768f;
            Seek(0);
        }
        catch { _file.Dispose(); throw; }
    }
    private long FindEdge(bool reverse)
    {
        var frames = _file.Length / 4;
        for (long block = 0; block < frames; block += _buffer.Length / 4)
        {
            var length = (int)Math.Min(_buffer.Length / 4, frames - block);
            var position = reverse ? frames - block - length : block;
            _file.Position = position * 4; _file.ReadExactly(_buffer.AsSpan(0, length * 4));
            for (var n = 0; n < length; n++)
            {
                var frame = reverse ? length - 1 - n : n;
                var l = BinaryPrimitives.ReadInt16LittleEndian(_buffer.AsSpan(frame * 4));
                var r = BinaryPrimitives.ReadInt16LittleEndian(_buffer.AsSpan(frame * 4 + 2));
                if (Math.Abs((int)l) > 8 || Math.Abs((int)r) > 8) return position + frame;
            }
        }
        return reverse ? 0 : frames;
    }
    public void Seek(long frame)
    {
        _position = Math.Clamp(frame, 0, Frames - 1); _file.Position = (_start + _position) * 4; _offset = _count = 0;
    }
    public void ReadFrame(out float left, out float right)
    {
        if (_position >= Frames) Seek(_overlap);
        if (_offset >= _count)
        {
            _count = (int)Math.Min(_buffer.Length, (Frames - _position) * 4);
            _file.ReadExactly(_buffer.AsSpan(0, _count)); _offset = 0;
        }
        left = BinaryPrimitives.ReadInt16LittleEndian(_buffer.AsSpan(_offset)) / 32768f;
        right = BinaryPrimitives.ReadInt16LittleEndian(_buffer.AsSpan(_offset + 2)) / 32768f;
        if (_position >= Frames - _overlap)
        {
            var index = (int)(_position - (Frames - _overlap));
            var amount = index / (double)_overlap;
            var outgoing = (float)Math.Cos(amount * Math.PI / 2); var incoming = (float)Math.Sin(amount * Math.PI / 2);
            left = left * outgoing + _head[index * 2] * incoming;
            right = right * outgoing + _head[index * 2 + 1] * incoming;
        }
        _offset += 4; _position++;
    }
    public void Dispose() => _file.Dispose();
}

/// <summary>One stereo mix, independent channel gains, sample-accurate ramps and no audio-thread allocations.</summary>
internal sealed class AudioMixer : IDisposable
{
    private sealed class MusicVoice(PcmLoop source)
    {
        public PcmLoop Source = source;
        public float Gain, Target, Step;
        public int Remaining;
        public void Fade(float target, int frames) { Target = target; Remaining = frames; Step = (target - Gain) / Math.Max(1, frames); }
    }
    private sealed class Effect(float[] samples) { public float[] Samples = samples; public int Position; }
    private readonly List<MusicVoice> _music = [];
    private readonly List<Effect> _effects = [];
    private float _musicGain = .25f, _soundGain = .5f, _master = 1, _masterTarget = 1, _masterStep;
    private int _masterRemaining;
    public int MusicVoices => _music.Count;
    public int EffectVoices => _effects.Count;
    public bool HasAudibleWork => _musicGain > 0 && _music.Count > 0 || _effects.Count > 0 || _masterRemaining > 0;
    public void SetVolumes(float music, float sounds) { _musicGain = Math.Clamp(music, 0, 1); _soundGain = Math.Clamp(sounds, 0, 1); }
    public void SetMaster(float target, int milliseconds)
    {
        _masterTarget = target; _masterRemaining = milliseconds * 48;
        _masterStep = (target - _master) / Math.Max(1, _masterRemaining);
        if (_masterRemaining == 0) _master = target;
    }
    public void ChangeMusic(PcmLoop source)
    {
        // A quick succession of choices remains bounded and never restarts an identical active track.
        while (_music.Count >= 3) { _music[0].Source.Dispose(); _music.RemoveAt(0); }
        foreach (var voice in _music) voice.Fade(0, 48000);
        var next = new MusicVoice(source); next.Fade(1, 48000); _music.Add(next);
    }
    public void StopMusic(int milliseconds = 250) { foreach (var voice in _music) voice.Fade(0, milliseconds * 48); }
    public void EffectSound(float[] sound)
    {
        if (_soundGain <= 0) return;
        if (_effects.Count >= 12) _effects.RemoveAt(0);
        _effects.Add(new Effect(sound));
    }
    public void ClearEffects() => _effects.Clear();
    public void Render(Span<float> output)
    {
        output.Clear();
        for (var frame = 0; frame < output.Length / 2; frame++)
        {
            float left = 0, right = 0;
            for (var n = _music.Count - 1; n >= 0; n--)
            {
                var voice = _music[n];
                if (voice.Remaining > 0) { voice.Gain += voice.Step; if (--voice.Remaining == 0) voice.Gain = voice.Target; }
                if (voice.Gain <= 0 && voice.Target <= 0) { voice.Source.Dispose(); _music.RemoveAt(n); continue; }
                voice.Source.ReadFrame(out var l, out var r);
                left += l * voice.Gain * _musicGain; right += r * voice.Gain * _musicGain;
            }
            for (var n = _effects.Count - 1; n >= 0; n--)
            {
                var voice = _effects[n];
                if (voice.Position >= voice.Samples.Length) { _effects.RemoveAt(n); continue; }
                left += voice.Samples[voice.Position++] * _soundGain;
                right += voice.Samples[voice.Position++] * _soundGain;
            }
            if (_masterRemaining > 0) { _master += _masterStep; if (--_masterRemaining == 0) _master = _masterTarget; }
            output[frame * 2] = Math.Clamp(left * _master, -.98f, .98f);
            output[frame * 2 + 1] = Math.Clamp(right * _master, -.98f, .98f);
        }
    }
    public void Dispose() { foreach (var voice in _music) voice.Source.Dispose(); _music.Clear(); _effects.Clear(); }
}

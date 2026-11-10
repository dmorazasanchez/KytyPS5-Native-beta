using System.Text.Json;
using System.Text.Json.Serialization;

namespace KytyPS5;

internal sealed class AudioPreferences
{
    public bool MusicEnabled { get; set; } = true;
    public string Theme { get; set; } = "fairy";
    public int MusicVolume { get; set; } = 25;
    public bool SoundsEnabled { get; set; } = true;
    public int SoundsVolume { get; set; } = 50;
    public Dictionary<string, string> Imports { get; set; } = [];
    [JsonExtensionData] public Dictionary<string, JsonElement>? AdditionalProperties { get; set; }
    public void Normalize()
    {
        MusicVolume = Math.Clamp(MusicVolume, 0, 100); SoundsVolume = Math.Clamp(SoundsVolume, 0, 100);
        if (!MusicLibrary.Themes.Any(t => t.Id == Theme)) Theme = "fairy";
        Imports ??= [];
    }
}

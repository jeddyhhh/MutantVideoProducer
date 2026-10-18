# Audio finishing

Open **Tools > Audio Finishing…**, or **Finish mix…** on the Audio Mixer's master strip.

1. Choose a target, or enter your own integrated LUFS and true-peak ceiling.
2. Press **Analyze mix**. A cancellable scan measures the entire project, including track/event effects, Master FX, fades, automation and ducking.
3. Review integrated loudness, loudness range, maximum momentary/short-term loudness, sample peaks, true peaks and stereo correlation.
4. Check the warnings: full-scale samples, peaks above the chosen ceiling, left/right imbalance, negative stereo correlation, long near-silent sections, mute/solo states and missing sources.
5. **Apply safe gain** adds one undoable Gain effect to Master FX. Without a limiter it goes at the end, preserves dynamics and limits gain to the available true-peak headroom, including a small safety margin. If that cannot reach the loudness target, add a **Limiter** to Master FX and apply again: the gain then goes *in front of* the limiter, which holds the peaks, and it reaches the target (up to +12 dB). Analyze again afterwards.

Analysis uses the same stereo mixer as playback/export and FFmpeg's
[EBU R128 scanner](https://ffmpeg.org/ffmpeg-filters.html#ebur128), including oversampled true-peak measurement.
It measures the mixed PCM before encoding; a lossy encoder can introduce additional peaks, so check the delivered file as well.

The supplied -16/-14/-23/-24 LUFS targets are editable starting points, not guarantees about a platform's current delivery requirements. Use your destination's specification. Very short material, silence and signals below the loudness gate cannot receive meaningful programme normalization. Invalid samples and cancelled scans cannot be applied.

Changing the project or its media makes measurements stale and disables gain application. **Save report…** writes JSON with the measurements, target, warnings and stale flag. Reanalyze after every mix adjustment. Measurements are session-local and never shared between projects.

## MCP

```json
{"action":"analyze","lufs":-16,"true_peak":-1.5}
```

Call `audio_finishing` with this object. `apply_gain` performs a fresh analysis and applies the safe gain in one undo step. It refuses to modify the project if the project changed during the scan. The response includes `integrated_lufs`, `true_peak_dbtp`, `range_lu`, `safe_gain_db` and `warnings`; after applying, `reanalyze_required` is true and `before_limiter` says whether the gain went in front of a Master FX Limiter.

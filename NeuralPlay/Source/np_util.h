#pragma once
// ─────────────────────────────────────────────────────────────
// np_util.h — Utilidades de NeuralPlay (audio, formato, rutas, dispositivo).
// Extraído de Main.cpp sin cambios de comportamiento.
// ─────────────────────────────────────────────────────────────
#include <JuceHeader.h>
#include <cmath>

static inline float softClip (float x) noexcept
{
    const float t = 0.9f;
    if (x >  t) return  t + (1.0f - t) * std::tanh ((x - t) / (1.0f - t));
    if (x < -t) return -t + (1.0f - t) * std::tanh ((x + t) / (1.0f - t));
    return x;
}
static inline float dbToGain (float db) noexcept
{
    return db <= -59.95f ? 0.0f : std::pow (10.0f, db * 0.05f);
}
static juce::String dbText (double v)
{
    if (v <= -59.95) return juce::String ("-inf");
    return juce::String (v > 0.0 ? "+" : "") + juce::String (v, 1);
}
static juce::String fmtTime (double s)
{
    if (s < 0) s = 0;
    const int m = (int) (s / 60.0);
    const int sec = (int) std::fmod (s, 60.0);
    return juce::String (m) + ":" + juce::String (sec).paddedLeft ('0', 2);
}
static juce::File npAppDir()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
             .getChildFile ("Library").getChildFile ("Application Support").getChildFile ("NeuralPlay");
}
static juce::File npCacheDir() { return npAppDir().getChildFile ("cache"); }

// ── Dispositivo: en iPhone la interfaz es compacta y las salidas se capan a 4 ──
static bool npEsIPhone()
{
   #if JUCE_IOS
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        auto r = d->userArea;                                   // en puntos lógicos
        const int menor = juce::jmin (r.getWidth(), r.getHeight());
        return menor > 0 && menor < 500;                        // iPhone <500pt · iPad >=744pt
    }
   #endif
    return false;
}
static constexpr int NP_MAX_SALIDAS_IPHONE = 4;                 // interfaz chica
static int npCapSalidas (int deseadas)                          // límite por dispositivo
{
    return npEsIPhone() ? juce::jmin (deseadas, NP_MAX_SALIDAS_IPHONE) : deseadas;
}
// Sensibilidad del arrastre sobre el mapa: en táctil (iPhone/iPad) más ágil que en Mac.
static double npMapDragSens()
{
   #if JUCE_IOS || JUCE_ANDROID
    return 1.15;
   #else
    return 0.5;
   #endif
}
static juce::int64 npFolderSize (const juce::File& f)
{
    juce::int64 s = 0;
    for (auto& c : f.findChildFiles (juce::File::findFiles, true)) s += c.getSize();
    return s;
}
static juce::String npFmtBytes (juce::int64 b)
{
    if (b >= 1073741824LL) return juce::String (b / 1073741824.0, 2) + " GB";
    if (b >= 1048576LL)    return juce::String (b / 1048576.0, 1) + " MB";
    if (b >= 1024LL)       return juce::String ((double) (b / 1024LL), 0) + " KB";
    return juce::String (b) + " B";
}

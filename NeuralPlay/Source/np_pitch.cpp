#include "np_pitch.h"
#include "signalsmith-stretch.h"   // vendorizado en third_party/signalsmith

// ─────────────────────────────────────────────────────────────────────────────
// Implementación concreta con Signalsmith Stretch (C++ header-only, licencia MIT).
// Es la única parte que "conoce" a Signalsmith; el resto de la app habla solo con
// la interfaz PitchShifter. Para migrar a zplane élastique se agrega otra clase
// aquí y se cambia createDefault().
// ─────────────────────────────────────────────────────────────────────────────
class SignalsmithPitchShifter : public PitchShifter
{
public:
    void prepare (int channels, double sampleRate, int /*maxBlock*/) override
    {
        channels_ = channels;
        sampleRate_ = sampleRate;
        stretch_.presetDefault (channels, (float) sampleRate);   // preset de calidad
        stretch_.reset();
    }

    void setSemitones (float semitones) override
    {
        stretch_.setTransposeSemitones (semitones);
    }

    int latencySamples() const override
    {
        return stretch_.inputLatency() + stretch_.outputLatency();
    }

    void reset() override { stretch_.reset(); }

    void process (const float* const* in, float* const* out, int numSamples) override
    {
        // Mismo número de samples de entrada y salida = pitch-shift sin cambiar la duración.
        stretch_.process (in, numSamples, out, numSamples);
    }

private:
    signalsmith::stretch::SignalsmithStretch<float> stretch_;
    int channels_ = 0;
    double sampleRate_ = 44100.0;
};

std::unique_ptr<PitchShifter> PitchShifter::createDefault()
{
    return std::make_unique<SignalsmithPitchShifter>();
}

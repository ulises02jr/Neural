#pragma once
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// Interfaz abstracta de transposición en tiempo real (pitch-shift sin cambiar el
// tempo). La idea es poder cambiar el MOTOR por debajo —hoy Signalsmith (MIT),
// mañana zplane élastique— sin tocar el resto del motor de audio de NeuralPlay.
//
// Regla de negocio (se aplica AFUERA, por stem): las familias Click, Guía,
// Batería, Percusión y Loops NO se transponen; a esos stems simplemente no se
// les pone un PitchShifter (se dejan al tono original).
// ─────────────────────────────────────────────────────────────────────────────
class PitchShifter
{
public:
    virtual ~PitchShifter() = default;

    // Prepara el motor: canales, sample-rate del dispositivo y tamaño máximo de bloque.
    virtual void prepare (int channels, double sampleRate, int maxBlock) = 0;

    // Semitonos de transposición. Puede cambiar en vivo. 0 = sin cambio.
    virtual void setSemitones (float semitones) = 0;

    // Latencia (en samples) que introduce el motor, por si hay que alinear.
    virtual int latencySamples() const = 0;

    // Limpia el estado interno (al hacer seek o cambiar de canción).
    virtual void reset() = 0;

    // Procesa un bloque manteniendo la duración (pitch-shift puro).
    // 'in' y 'out' son arreglos de punteros por canal; NO pueden ser el mismo buffer.
    virtual void process (const float* const* in, float* const* out, int numSamples) = 0;

    // Fábrica del motor por defecto (actualmente Signalsmith). Cambiar aquí el
    // día que migremos a élastique es todo lo que hará falta para intercambiarlo.
    static std::unique_ptr<PitchShifter> createDefault();
};

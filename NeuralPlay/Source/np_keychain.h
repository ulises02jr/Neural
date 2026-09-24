// np_keychain.h — Guardado seguro de credenciales (Keychain de iOS/macOS).
//
// Por qué existe: juce::TextEditor no puede usar el autofill nativo del sistema
// (a diferencia de NeuralCharts en Flutter). Para ofrecer "recordar contraseña"
// de forma SEGURA guardamos el correo y la contraseña en el Keychain del sistema
// (Security.framework), cifrado en reposo y aislado por app en iOS.
//
// Solo se declara aquí; la implementación vive en np_keychain.cpp, donde los
// headers de Security se incluyen ANTES que JUCE para evitar el choque del tipo
// 'Point' (Carbon/MacTypes) con juce::Point. En plataformas no-Apple los tres
// métodos son un no-op seguro (no se guarda nada).
#pragma once
#include <JuceHeader.h>

namespace npkc
{
    bool         set    (const juce::String& key, const juce::String& value);
    juce::String get    (const juce::String& key);
    void         remove (const juce::String& key);
}

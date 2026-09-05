#include <JuceHeader.h>
#include "BinaryData.h"
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <array>

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
static juce::String httpGet (const juce::String& url, const juce::String& token)
{
    juce::URL u (url);
    int status = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withExtraHeaders ("Authorization: Bearer " + token)
                    .withConnectionTimeoutMs (15000)
                    .withStatusCode (&status);
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr) return {};
    return in->readEntireStreamAsString();
}
static bool httpDownload (const juce::String& url, const juce::String& token, const juce::File& dest,
                          std::function<void (double)> onProgress = {},
                          std::function<bool()> cancel = {})
{
    juce::URL u (url);
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withExtraHeaders ("Authorization: Bearer " + token)
                    .withConnectionTimeoutMs (30000);
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr) return false;
    const juce::int64 expected = in->getTotalLength();   // -1 si el server no da Content-Length
    dest.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp (dest);
    juce::int64 written = 0;
    {
        std::unique_ptr<juce::FileOutputStream> out (tmp.getFile().createOutputStream());
        if (out == nullptr) return false;
        juce::HeapBlock<char> buf (1 << 16);
        while (! in->isExhausted())
        {
            if (cancel && cancel()) return false;               // salida rápida (cierre de la app)
            const int got = in->read (buf, 1 << 16);
            if (got <= 0) break;
            out->write (buf, (size_t) got);
            written += got;
            if (onProgress && expected > 0) onProgress ((double) written / (double) expected);
        }
    }
    if (expected > 0 && written != expected) return false;      // descarga incompleta -> descartar el temporal
    return tmp.overwriteTargetFileWithTemporary();
}

static juce::String httpPostJson (const juce::String& url, const juce::String& jsonBody)
{
    juce::URL u = juce::URL (url).withPOSTData (jsonBody);
    int status = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                    .withExtraHeaders ("Content-Type: application/json")
                    .withConnectionTimeoutMs (8000)
                    .withStatusCode (&status);
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr) return {};
    return in->readEntireStreamAsString();
}

// POST con parámetros de formulario + token en header Bearer (para /api/live/setlist/* y render)
static juce::String httpPostForm (const juce::String& baseUrl, juce::StringPairArray params, const juce::String& token)
{
    juce::URL u (baseUrl);
    params.set ("_", "1");   // asegura que JUCE use método POST (cuerpo no vacío)
    for (auto& k : params.getAllKeys()) u = u.withParameter (k, params[k]);
    int status = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                    .withExtraHeaders ("Authorization: Bearer " + token)
                    .withConnectionTimeoutMs (10000)
                    .withStatusCode (&status);
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr) return {};
    return in->readEntireStreamAsString();
}

static juce::String localLanIp()
{
    for (auto& a : juce::IPAddress::getAllAddresses (false))   // IPv4
        if (! a.isNull() && a.toString() != "127.0.0.1")
            return a.toString();
    return "127.0.0.1";
}

struct MidiNoteEv { double seg = 0.0; int note = 0; int vel = 100; };
struct MidiBox { juce::String id, nombre; int canal = 1; juce::Array<MidiNoteEv> notas; };

struct SongEntry
{
    int id = 0, tono = 0, beatsPerBar = 4;
    double tempo = 0.0;
    juce::String compas = "4/4";
    juce::var mix;                    // mezcla guardada de esta canción en el repertorio
    juce::String titulo, artista, tonoNombre;
    juce::File folder;
    juce::Array<double> secTimes;
    juce::StringArray secNames;
    juce::StringArray famFiles, famNames;   // familia por stem (del servidor)
    juce::Array<MidiBox> midiBoxes;         // cajas MIDI + notas (del servidor)
    juce::Array<double> beatGrid;           // negras (seg) para el MIDI clock variable
    juce::String portada;
    juce::File coverFile;
    juce::Image cover;
};

struct FaderLNF : public juce::LookAndFeel_V4
{
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float,
                           const juce::Slider::SliderStyle, juce::Slider& slider) override
    {
        const float cx = x + width * 0.5f;
        const float top = (float) y + 4.0f;
        const float bot = (float) y + height - 4.0f;

        // Columna (groove) mas gruesa
        juce::Rectangle<float> groove (cx - 3.5f, top, 7.0f, bot - top);
        g.setColour (juce::Colour (0xff0c0c0c));
        g.fillRoundedRectangle (groove, 3.5f);
        g.setColour (juce::Colour (0xff313131));
        g.drawRoundedRectangle (groove, 3.5f, 1.0f);

        // Alumbrado VU (blanco) segun intensidad que suena, de abajo hacia arriba
        const float raw = (float) (double) slider.getProperties().getWithDefault ("lvl", 0.0);
        const float lvl = std::pow (juce::jlimit (0.0f, 1.0f, raw * 2.4f), 0.6f);
        if (lvl > 0.02f)
        {
            const float gh   = (bot - top - 3.0f) * lvl;
            const float yTop = bot - 1.5f - gh;
            juce::Rectangle<float> vu (cx - 3.5f, yTop, 7.0f, gh);
            juce::ColourGradient vg (juce::Colours::white.withAlpha (0.06f), cx, yTop,
                                     juce::Colours::white.withAlpha (0.92f), cx, bot, false);
            vg.addColour (0.5, juce::Colours::white.withAlpha (0.5f));
            g.setGradientFill (vg);
            g.fillRoundedRectangle (vu, 3.5f);
            // nucleo brillante
            g.setColour (juce::Colours::white.withAlpha (0.55f * lvl));
            g.fillRoundedRectangle (cx - 1.5f, yTop, 3.0f, gh, 1.5f);
        }

        // Escala medible: solo marcas laterales, separadas de la columna (efecto flotante)
        const int divs = 10;
        const float gEdge = 3.5f;   // media columna
        const float gap   = 2.5f;   // separacion flotante
        for (int i = 0; i <= divs; ++i)
        {
            const float ty   = juce::jmap ((float) i, 0.0f, (float) divs, top + 2.0f, bot - 2.0f);
            const bool major = (i % 5 == 0);
            const float len  = major ? 7.0f : 4.5f;
            const float th   = major ? 1.5f : 1.0f;
            g.setColour (major ? juce::Colour (0xff5f5f5f) : juce::Colour (0xff383838));
            g.fillRect (cx - gEdge - gap - len, ty - th * 0.5f, len, th);
            g.fillRect (cx + gEdge + gap,       ty - th * 0.5f, len, th);
        }

        // Nivel bajo el handle (tenue dorado)
        if (bot - sliderPos > 1.0f)
        {
            g.setColour (juce::Colour (0x1fffffff));
            g.fillRoundedRectangle (juce::Rectangle<float> (cx - 3.0f, sliderPos, 6.0f, bot - sliderPos), 3.0f);
        }

        // Handle grande y tecnologico
        const float capW = juce::jmin ((float) width - 4.0f, 48.0f);
        const float capH = 26.0f;
        juce::Rectangle<float> cap (cx - capW * 0.5f, sliderPos - capH * 0.5f, capW, capH);
        juce::ColourGradient cg (juce::Colour (0xff6d6d6d), cap.getX(), cap.getY(),
                                 juce::Colour (0xff1c1c1c), cap.getX(), cap.getBottom(), false);
        cg.addColour (0.48, juce::Colour (0xff3a3a3a));
        cg.addColour (0.52, juce::Colour (0xff2c2c2c));
        g.setGradientFill (cg);
        g.fillRoundedRectangle (cap, 5.0f);
        g.setColour (juce::Colour (0xff121212));
        g.drawRoundedRectangle (cap, 5.0f, 1.3f);
        // brillo superior
        g.setColour (juce::Colour (0x50ffffff));
        g.drawLine (cap.getX() + 6.0f, cap.getY() + 2.2f, cap.getRight() - 6.0f, cap.getY() + 2.2f, 1.0f);
        // ranuras tipo agarre
        g.setColour (juce::Colour (0x35000000));
        g.fillRect (cap.getX() + 7.0f, cap.getCentreY() - 6.0f, cap.getWidth() - 14.0f, 1.0f);
        g.fillRect (cap.getX() + 7.0f, cap.getCentreY() + 6.0f, cap.getWidth() - 14.0f, 1.0f);
        // indicador central: azul normal, rojo si el canal esta muteado
        const bool muted = (bool) slider.getProperties().getWithDefault ("muted", false);
        g.setColour (muted ? juce::Colour (0xffE0433E) : juce::Colour (0xff2E8BFF));
        g.fillRoundedRectangle (cap.getX() + 6.0f, cap.getCentreY() - 1.75f, cap.getWidth() - 12.0f, 3.5f, 1.75f);
    }
};

struct PillLNF : public juce::LookAndFeel_V4
{
    PillLNF()
    {
        setDefaultSansSerifTypefaceName ("Helvetica Neue");
        setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
    }
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                               bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        auto c = bg;
        if (down) c = c.brighter (0.06f); else if (over) c = c.brighter (0.10f);
        const float rad = juce::jmin (r.getHeight() * 0.5f, 12.0f);
        g.setColour (c);
        g.fillRoundedRectangle (r, rad);
        g.setColour (juce::Colour (0xff2a2a2a));
        g.drawRoundedRectangle (r, rad, 1.0f);
    }
    juce::Font getTextButtonFont (juce::TextButton&, int h) override
    {
        return juce::Font (juce::jmin (14.5f, (float) h * 0.4f), juce::Font::bold);
    }
};

static void drawNPLogoBig (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto c = area.getCentre();
    const float bw = 13.0f, gap = 9.0f, h = 92.0f;
    const float hs[6] = { 0.36f, 0.70f, 1.0f, 0.5f, 0.86f, 0.44f };
    const float totalW = 6.0f * bw + 5.0f * gap;
    const float x0 = c.x - totalW * 0.5f;
    const float top = c.y - 96.0f;
    g.setColour (juce::Colour (0xfff2f2f2));
    for (int i = 0; i < 6; ++i)
    {
        const float bh = h * hs[i];
        g.fillRoundedRectangle (x0 + i * (bw + gap), top + (h - bh), bw, bh, bw * 0.5f);
    }
    g.setColour (juce::Colour (0xfff2f2f2));
    g.setFont (juce::Font (30.0f));
    g.drawText ("Neural", (int) area.getX(), (int) (top + h + 18), (int) area.getWidth(), 34, juce::Justification::centredTop);
    g.setFont (juce::Font (34.0f, juce::Font::bold));
    g.drawText ("Play", (int) area.getX(), (int) (top + h + 52), (int) area.getWidth(), 40, juce::Justification::centredTop);
}

struct SplashComp : public juce::Component
{
    juce::Image logo;
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0a0a));
        if (logo.isValid())
        {
            const float w = juce::jmin (400.0f, getWidth() * 0.62f);
            const float h = w * (float) logo.getHeight() / (float) juce::jmax (1, logo.getWidth());
            g.drawImage (logo, juce::Rectangle<float> (getWidth() * 0.5f - w * 0.5f, getHeight() * 0.5f - h * 0.5f, w, h),
                         juce::RectanglePlacement::centred);
        }
        else drawNPLogoBig (g, getLocalBounds().toFloat());
    }
};

// Boton de Play/Pausa: dibuja el simbolo (triangulo / dos barras) en azul.
// Decide el icono segun su texto ("Pausa" => pausa, si no => play), asi
// reutiliza los setButtonText existentes sin tocar la logica de reproduccion.
struct PlayIconButton : public juce::Button
{
    PlayIconButton() : juce::Button ("play") {}
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float rad = juce::jmin (r.getHeight() * 0.5f, 12.0f);
        juce::Colour blue (0xff2E6BE6);
        if (! isEnabled())  blue = blue.withAlpha (0.35f);
        else if (down)      blue = blue.darker (0.10f);
        else if (over)      blue = blue.brighter (0.12f);
        g.setColour (blue);
        g.fillRoundedRectangle (r, rad);

        const bool pause = getButtonText().equalsIgnoreCase ("Pausa");
        g.setColour (juce::Colours::white.withAlpha (isEnabled() ? 1.0f : 0.6f));
        const auto c = r.getCentre();
        const float s = juce::jmin (r.getHeight(), r.getWidth());
        if (pause)
        {
            const float bw = s * 0.13f, gap = s * 0.14f, bh = s * 0.42f;
            g.fillRoundedRectangle (c.x - gap - bw, c.y - bh * 0.5f, bw, bh, bw * 0.35f);
            g.fillRoundedRectangle (c.x + gap,       c.y - bh * 0.5f, bw, bh, bw * 0.35f);
        }
        else
        {
            const float w = s * 0.34f, h = s * 0.42f;
            juce::Path tri;
            tri.addTriangle (c.x - w * 0.40f, c.y - h * 0.5f,
                             c.x - w * 0.40f, c.y + h * 0.5f,
                             c.x + w * 0.60f, c.y);
            g.fillPath (tri);
        }
    }
};

// Botón de Desvanecer: dibuja una rampa descendente (fade out) en un pill oscuro.
struct FadeIconButton : public juce::Button
{
    FadeIconButton() : juce::Button ("fade") {}
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float rad = juce::jmin (r.getHeight() * 0.5f, 12.0f);
        juce::Colour bg (0xff1f1f1f);
        if (down) bg = bg.brighter (0.06f); else if (over) bg = bg.brighter (0.10f);
        g.setColour (bg); g.fillRoundedRectangle (r, rad);
        g.setColour (juce::Colour (0xff2a2a2a)); g.drawRoundedRectangle (r, rad, 1.0f);

        const auto c = r.getCentre();
        const float hw = juce::jmin (r.getWidth() * 0.30f, 15.0f);
        const float hh = juce::jmin (r.getHeight() * 0.30f, 10.0f);
        juce::Path ramp;                    // rampa que baja: alto a la izquierda, cero a la derecha
        ramp.startNewSubPath (c.x - hw, c.y - hh);
        ramp.lineTo         (c.x - hw, c.y + hh);
        ramp.lineTo         (c.x + hw, c.y + hh);
        ramp.closeSubPath();
        g.setColour (isEnabled() ? juce::Colours::white.withAlpha (0.92f) : juce::Colours::white.withAlpha (0.4f));
        g.fillPath (ramp);
    }
};

struct SongCard : public juce::Component
{
    juce::Image cover;
    juce::String titulo, tono;
    bool active = false;
    bool editMode = false;
    int index = 0;
    int songId = 0;   // #4 id de la canción (para mapping de teclado)
    std::function<void()> onClick, onRemove, onTono, onAddAfter;
    std::function<void (int fromIndex, int toIndex)> onReorder;   // arrastrar para reordenar
    float dlProgress = -1.0f;   // -1 = sin barra; 0..1 = descargando

    int homeX = 0; bool dragging = false;

    juce::Rectangle<float> coverRect() const
    {
        auto r = getLocalBounds().toFloat();
        r.removeFromTop (6.0f);
        return r.removeFromTop (r.getHeight() * 0.76f).reduced (1.0f);
    }
    juce::Rectangle<float> removeBtnRect() const { auto c = coverRect(); return { c.getRight() - 34.0f, c.getY() + 8.0f, 26.0f, 26.0f }; }
    juce::Rectangle<float> tonoBtnRect()   const { auto c = coverRect(); return { c.getCentreX() - 22.0f, c.getCentreY() - 18.0f, 44.0f, 36.0f }; }
    juce::Rectangle<float> addBtnRect()    const { auto c = coverRect(); return { c.getRight() - 34.0f, c.getBottom() - 34.0f, 26.0f, 26.0f }; }

    void mouseDown (const juce::MouseEvent&) override
    {
        homeX = getX(); dragging = false;
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! editMode) return;
        const int dx = e.getDistanceFromDragStartX();
        if (dragging || std::abs (dx) > 5)
        {
            dragging = true;
            toFront (false);
            setTopLeftPosition (homeX + dx, getY());
        }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (editMode && dragging)
        {
            dragging = false;
            const int step  = juce::jmax (1, getWidth() + 10);
            const int steps = juce::roundToInt ((float) e.getDistanceFromDragStartX() / (float) step);
            if (onReorder) onReorder (index, index + steps);   // el contenedor re-acomoda y snap
            return;
        }
        if (! e.mouseWasClicked()) return;
        if (editMode)
        {
            if (removeBtnRect().contains (e.position)) { if (onRemove)   onRemove();   return; }
            if (addBtnRect().contains (e.position))    { if (onAddAfter) onAddAfter(); return; }
            if (tonoBtnRect().contains (e.position))   { if (onTono)     onTono();     return; }
        }
        if (onClick) onClick();
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        r.removeFromTop (6.0f);
        auto cov = r.removeFromTop (r.getHeight() * 0.76f).reduced (1.0f);
        {
            juce::Path clip; clip.addRoundedRectangle (cov, 9.0f);
            g.saveState(); g.reduceClipRegion (clip);
            if (cover.isValid()) g.drawImage (cover, cov, juce::RectanglePlacement::fillDestination);
            else { g.setColour (juce::Colour (0xff1f1f1f)); g.fillRect (cov); }
            g.restoreState();
        }
        g.setColour (active ? juce::Colour (0xffffffff) : juce::Colour (0xff2a2a2a));
        g.drawRoundedRectangle (cov, 9.0f, active ? 2.5f : 1.0f);
        r.removeFromTop (5.0f);   // aire entre la portada y el titulo
        auto txt = r.reduced (3.0f, 0.0f);
        juce::String linea = titulo;
        if (tono.isNotEmpty()) linea << "  (" << tono << ")";
        g.setColour (active ? juce::Colour (0xffffffff) : juce::Colour (0xfff2f2f2));
        g.setFont (juce::Font (17.0f, juce::Font::bold));
        g.drawFittedText (linea, txt.toNearestInt(), juce::Justification::topLeft, 2);

        if (editMode)
        {
            auto c = coverRect();
            { juce::Path clip; clip.addRoundedRectangle (c, 9.0f); g.saveState(); g.reduceClipRegion (clip);
              g.setColour (juce::Colour (0x66000000)); g.fillRect (c); g.restoreState(); }
            auto rb = removeBtnRect();
            g.setColour (juce::Colour (0xffE5534B)); g.fillEllipse (rb);
            g.setColour (juce::Colours::white); g.setFont (juce::Font (22.0f, juce::Font::bold));
            g.drawText (juce::String::fromUTF8 ("\xe2\x88\x92"), rb, juce::Justification::centred);   // −
            auto tb = tonoBtnRect();
            g.setColour (juce::Colour (0xF0141414)); g.fillRoundedRectangle (tb, 8.0f);
            g.setColour (juce::Colour (0x44ffffff)); g.drawRoundedRectangle (tb, 8.0f, 1.2f);
            g.setColour (juce::Colours::white); g.setFont (juce::Font (22.0f, juce::Font::bold));
            g.drawText (juce::String::fromUTF8 ("\xe2\x8b\xaf"), tb, juce::Justification::centred);   // ⋯
            auto ab = addBtnRect();   // + para agregar una canción después de esta
            g.setColour (juce::Colour (0xff3ED66E)); g.fillEllipse (ab);
            g.setColour (juce::Colours::white); g.setFont (juce::Font (23.0f, juce::Font::bold));
            g.drawText ("+", ab.translated (0.0f, -1.0f), juce::Justification::centred);
        }

        if (dlProgress >= 0.0f && dlProgress < 1.0f)   // barra de descarga sobre la portada
        {
            auto c = coverRect();
            { juce::Path clip; clip.addRoundedRectangle (c, 9.0f); g.saveState(); g.reduceClipRegion (clip);
              g.setColour (juce::Colour (0x99000000)); g.fillRect (c); g.restoreState(); }
            auto bar = juce::Rectangle<float> (c.getX() + 18.0f, c.getCentreY() - 4.0f, c.getWidth() - 36.0f, 8.0f);
            g.setColour (juce::Colour (0x33ffffff)); g.fillRoundedRectangle (bar, 4.0f);
            g.setColour (juce::Colour (0xff2E6BE6));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, dlProgress)), 4.0f);
        }
    }
};

struct AddCard : public juce::Component
{
    std::function<void()> onClick;
    void mouseUp (const juce::MouseEvent& e) override { if (onClick && e.mouseWasClicked()) onClick(); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        r.removeFromTop (6.0f);
        auto cov = r.removeFromTop (r.getHeight() * 0.76f).reduced (1.0f);
        g.setColour (juce::Colour (0xff121212)); g.fillRoundedRectangle (cov, 9.0f);
        g.setColour (juce::Colour (0x55ffffff)); g.drawRoundedRectangle (cov, 9.0f, 1.4f);
        g.setColour (juce::Colour (0xff7Cc6ff)); g.setFont (juce::Font (46.0f, juce::Font::bold));
        g.drawText ("+", cov, juce::Justification::centred);
        r.removeFromTop (5.0f);
        g.setColour (juce::Colour (0xffa3a3a3)); g.setFont (juce::Font (15.0f, juce::Font::bold));
        g.drawFittedText (juce::String::fromUTF8 ("Agregar canci\xc3\xb3n"), r.reduced (3.0f, 0.0f).toNearestInt(),
                          juce::Justification::topLeft, 2);
    }
};

struct ClickLabel : public juce::Label
{
    std::function<void()> onClick;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
};

struct SoloDot : public juce::Component
{
    bool on = false;
    std::function<void()> onClick;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float d = juce::jmin (b.getWidth(), 20.0f);
        juce::Rectangle<float> circ (b.getCentreX() - d * 0.5f, b.getY(), d, d);
        // columna propia del Solo (se ilumina en dorado si esta activo)
        juce::Rectangle<float> lane (b.getCentreX() - 3.0f, circ.getBottom() + 5.0f, 6.0f,
                                     b.getBottom() - circ.getBottom() - 7.0f);
        g.setColour (juce::Colour (0xff0c0c0c));
        g.fillRoundedRectangle (lane, 3.0f);
        g.setColour (juce::Colour (0xff262626));
        g.drawRoundedRectangle (lane, 3.0f, 1.0f);
        if (on)
        {
            juce::ColourGradient gg (juce::Colour (0x22ffffff), lane.getCentreX(), lane.getY(),
                                     juce::Colour (0xffffffff), lane.getCentreX(), lane.getBottom(), false);
            gg.addColour (0.5, juce::Colour (0xaaffffff));
            g.setGradientFill (gg);
            g.fillRoundedRectangle (lane.reduced (0.6f), 3.0f);
        }
        // boton "S"
        g.setColour (on ? juce::Colour (0xffffffff) : juce::Colour (0xff1c1c1c));
        g.fillEllipse (circ);
        g.setColour (on ? juce::Colour (0xff0a0a0a) : juce::Colour (0xff6f6f6f));
        g.drawEllipse (circ, 1.3f);
        g.setColour (on ? juce::Colour (0xff0a0a0a) : juce::Colour (0xffbcbcbc));
        g.setFont (juce::Font (11.5f, juce::Font::bold));
        g.drawText ("S", circ, juce::Justification::centred);
    }
};

static juce::String familyFor (const juce::String& raw)
{
    const auto n = raw.toLowerCase();
    const auto tok = n.upToFirstOccurrenceOf (" ", false, false);
    auto C = [&] (const char* s) { return n.contains (s); };
    if (C("click") || C("cue") || C("metro"))                                   return juce::String::fromUTF8 ("Click");
    if (C("guide") || C("guia"))                                                return juce::String::fromUTF8 ("Gu\xc3\xad" "a");
    if (C("bass")  || C("bajo"))                                                return juce::String::fromUTF8 ("Bajo");
    if (C("loop"))                                                              return juce::String::fromUTF8 ("Loops");
    if (C("fx")||C("riser")||C("sweep")||C("impact")||C("whoosh")||C("uplift")) return juce::String::fromUTF8 ("FX");
    if (C("drum")||C("bater")||C("beat")||C("kick")||C("snare")||C("hat")||C("tom")) return juce::String::fromUTF8 ("Bater\xc3\xad" "a");
    if (C("perc")||C("shaker")||C("conga")||C("tambor")||C("clap")||C("pander")) return juce::String::fromUTF8 ("Percusi\xc3\xb3n");
    if (C("pad"))                                                               return juce::String::fromUTF8 ("Pad");
    if (C("piano")||C("rhodes")||C("wurli"))                                    return juce::String::fromUTF8 ("Piano");
    if (C("key")||C("teclad")||C("synth")||C("organ")|| tok=="kb")              return juce::String::fromUTF8 ("Teclados");
    if (C("acous")|| tok=="ag")                                                 return juce::String ("AG");
    if (C("guit")||C("gtr")||C("guitar")|| tok=="eg" || tok=="ge")              return juce::String ("GE");
    if (C("string")||C("cuerda")||C("viol")||C("cello"))                        return juce::String::fromUTF8 ("Cuerdas");
    if (C("sax")||C("trumpet")||C("trompet")||C("brass")||C("trombon"))         return juce::String::fromUTF8 ("Metales");
    if (C("voz")||C("vocal")||C("coro")||C("lead")||C("bgv")||C("choir")||C("voc")||C("alto")||C("tenor")||C("sopran")) return juce::String::fromUTF8 ("Voces");
    return juce::String::fromUTF8 ("Otros");
}

// ───────── Enrutamiento de salidas de audio por familia ─────────
static const char* kRouteFam[17] = {
    "Voces", "AG", "GE", "Piano", "Teclados", "Pad",
    "Cuerdas", "Metales", "Bajo", "Bater\xc3\xad" "a", "Percusi\xc3\xb3n", "Loops", "FX",
    "Gu\xc3\xad" "a", "M\xc3\xbasica original", "Click", "Otros" };
static constexpr int kNumFam = 17;

static int routeFamIndex (const juce::String& serverFam, const juce::String& trackName)
{
    juce::String fam = serverFam;
    if (fam.isEmpty()) fam = familyFor (trackName);
    if (fam.equalsIgnoreCase ("Guitarras") || fam.startsWithIgnoreCase ("Guitarra El")) fam = "GE";   // compat familias viejas
    if (fam.startsWithIgnoreCase ("Guitarra Ac")) fam = "AG";
    if (fam.equalsIgnoreCase ("Teclas"))    fam = juce::String::fromUTF8 ("Teclados");
    for (int i = 0; i < kNumFam; ++i)
        if (fam.equalsIgnoreCase (juce::String::fromUTF8 (kRouteFam[i]))) return i;
    return kNumFam - 1;   // Otros
}

struct FaderStripComp : public juce::Component
{
    std::function<void (juce::Graphics&)> onPaint;
    std::function<void (const juce::MouseEvent&)> onMouseDown;   // para armar faders en modo MIDI
    void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
    void mouseDown (const juce::MouseEvent& e) override { if (onMouseDown) onMouseDown (e); }
};

// Viewport con la rueda invertida (para que el desplazamiento de los tracks
// vaya en el sentido natural del trackpad, igual que el strip de canciones)
struct HScrollViewport : public juce::Viewport
{
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        juce::MouseWheelDetails w2 = w;
        w2.deltaX = -w.deltaX;
        w2.deltaY = -w.deltaY;
        juce::Viewport::mouseWheelMove (e, w2);
    }
};

struct IconButton : public juce::Button
{
    int kind = 0;          // 0 faders, 1 repeat, 2 infinito
    bool active = false;
    IconButton() : juce::Button ("") {}
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat();
        auto r = b.reduced (1.0f);
        g.setColour (active ? juce::Colour (0xff2E8BFF)
                            : (down ? juce::Colour (0xff2a2a2a) : (over ? juce::Colour (0xff262626) : juce::Colour (0xff1f1f1f))));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (juce::Colour (0x22ffffff));
        g.drawRoundedRectangle (r, 6.0f, 1.0f);

        const juce::Colour ic = active ? juce::Colours::white : juce::Colour (0xffe6e6e6);
        if (kind == 0)
        {
            // faders: 3 mini deslizadores
            const float top = b.getY() + b.getHeight() * 0.30f;
            const float bot = b.getY() + b.getHeight() * 0.70f;
            const float xs[3] = { b.getCentreX() - b.getWidth() * 0.22f, b.getCentreX(), b.getCentreX() + b.getWidth() * 0.22f };
            const float ky[3] = { 0.55f, 0.30f, 0.62f };
            for (int i = 0; i < 3; ++i)
            {
                g.setColour (juce::Colour (0x66ffffff));
                g.drawLine (xs[i], top, xs[i], bot, 1.6f);
                g.setColour (ic);
                const float y = juce::jmap (ky[i], top, bot);
                g.fillRoundedRectangle (xs[i] - 5.0f, y - 2.5f, 10.0f, 5.0f, 2.0f);
            }
        }
        else if (kind == 3)   // configuraciones: 3 rayitas
        {
            g.setColour (ic);
            const float w = b.getWidth() * 0.44f, cx = b.getCentreX();
            for (int i = 0; i < 3; ++i)
            {
                const float yy = b.getCentreY() + (i - 1) * (b.getHeight() * 0.17f);
                g.fillRoundedRectangle (cx - w * 0.5f, yy - 1.2f, w, 2.4f, 1.2f);
            }
        }
        else if (kind == 4)   // repertorios: lista con puntos
        {
            g.setColour (ic);
            const float cx = b.getCentreX(), lw = b.getWidth() * 0.34f;
            const float lx = cx - lw * 0.30f, dx = cx - lw * 0.72f;
            for (int i = 0; i < 3; ++i)
            {
                const float yy = b.getCentreY() + (i - 1) * (b.getHeight() * 0.17f);
                g.fillEllipse (dx - 1.7f, yy - 1.7f, 3.4f, 3.4f);
                g.fillRoundedRectangle (lx, yy - 1.1f, lw, 2.2f, 1.1f);
            }
        }
        else
        {
            g.setColour (ic);
            g.setFont (juce::Font (b.getHeight() * (kind == 2 ? 0.66f : 0.56f), juce::Font::bold));
            g.drawText (juce::String::fromUTF8 (kind == 2 ? "∞" : "↻"), b, juce::Justification::centred);
        }
    }
};

struct MixThumb : public juce::Thread
{
    MixThumb (juce::AudioThumbnail& t, juce::AudioFormatManager& fm,
              const juce::Array<juce::File>& f, double sr, long long len)
        : juce::Thread ("mixthumb"), thumb (t), fmt (fm), files (f), sampleRate (sr), total (len) {}
    ~MixThumb() override { stopThread (2000); }
    void run() override
    {
        if (total <= 0) return;
        thumb.reset (1, sampleRate, total);
        juce::OwnedArray<juce::AudioFormatReader> readers;
        for (auto& f : files) if (auto* r = fmt.createReaderFor (f)) readers.add (r);
        if (readers.isEmpty()) return;
        const int chunk = 1 << 16;
        juce::AudioBuffer<float> mix (1, chunk), tmp (2, chunk);
        long long pos = 0;
        while (pos < total && ! threadShouldExit())
        {
            const int n = (int) juce::jmin ((long long) chunk, total - pos);
            mix.clear();
            auto* m = mix.getWritePointer (0);
            for (auto* r : readers)
            {
                tmp.clear();
                r->read (&tmp, 0, n, pos, true, true);
                const float* a = tmp.getReadPointer (0);
                const float* b = tmp.getNumChannels() > 1 ? tmp.getReadPointer (1) : a;
                for (int i = 0; i < n; ++i) m[i] += 0.5f * (a[i] + b[i]);
            }
            for (int i = 0; i < n; ++i) { float v = m[i]; float sg = v < 0 ? -1.0f : 1.0f; m[i] = sg * std::pow (std::abs (v), 0.65f); }
            thumb.addBlock (pos, mix, 0, n);
            pos += (long long) n;
        }
    }
    juce::AudioThumbnail& thumb;
    juce::AudioFormatManager& fmt;
    juce::Array<juce::File> files;
    double sampleRate;
    long long total;
};

struct RepertoireLoader : public juce::Thread
{
    RepertoireLoader (juce::String url, juce::String tok, juce::File cache)
        : juce::Thread ("np-loader"), serverUrl (url), token (tok), cacheDir (cache) {}
    ~RepertoireLoader() override { stopThread (6000); }

    std::function<void (juce::String)> onStatus;
    std::function<void (juce::Array<SongEntry>)> onMeta;   // metadata + portadas lista (arma tarjetas)
    std::function<void (int, double)> onProgress;          // descarga: (indice de canción, fracción 0..1)
    std::function<void (juce::Array<SongEntry>)> onDone;

    void status (const juce::String& s)
    {
        if (onStatus) { auto cb = onStatus; juce::MessageManager::callAsync ([cb, s] { cb (s); }); }
    }
    void progress (int i, double f)
    {
        if (onProgress) { auto cb = onProgress; juce::MessageManager::callAsync ([cb, i, f] { cb (i, f); }); }
    }

    void run() override
    {
        status ("Conectando al servidor...");
        auto v = juce::JSON::parse (httpGet (serverUrl + "/api/live/setlists", token));
        if (! (bool) v.getProperty ("ok", false)) { status ("No se pudo conectar"); return; }

        auto* setlists = v.getProperty ("setlists", juce::var()).getArray();
        auto cIdx = v.getProperty ("canciones", juce::var());
        if (setlists == nullptr || setlists->isEmpty()) { status ("Sin repertorios"); return; }

        auto sl = (*setlists)[0];
        if (wantedId.isNotEmpty())
            for (auto& s : *setlists)
                if (s.getProperty ("id", "").toString() == wantedId) { sl = s; break; }
        resolvedId = sl.getProperty ("id", "").toString();
        juce::String slName = sl.getProperty ("nombre", "Repertorio").toString();
        resolvedName = slName;
        auto* cs = sl.getProperty ("canciones", juce::var()).getArray();
        if (cs == nullptr || cs->isEmpty())   // setlist vacío: cargarlo igual (para ir agregando)
        {
            status ("Repertorio vac\xc3\xado (agreg\xc3\xa1 canciones)");
            juce::Array<SongEntry> out;
            if (onDone) { auto cb = onDone; juce::MessageManager::callAsync ([cb, out] { cb (out); }); }
            return;
        }

        juce::Array<SongEntry> out;
        for (int i = 0; i < cs->size(); ++i)
        {
            if (threadShouldExit()) return;
            auto item = (*cs)[i];
            SongEntry e;
            e.id   = (int) item.getProperty ("id", 0);
            e.tono = (int) item.getProperty ("tono_semitonos", 0);   // semitono resuelto por el servidor
            e.mix  = item.getProperty ("mix", juce::var());          // mezcla guardada en el repertorio
            auto meta = cIdx.getProperty (juce::String (e.id), juce::var());
            e.titulo     = meta.getProperty ("titulo", "Cancion " + juce::String (e.id)).toString();
            e.artista    = meta.getProperty ("artista", "").toString();
            e.tonoNombre = meta.getProperty ("tono", "").toString();
            { auto tn = item.getProperty ("tono_nombre", "").toString(); if (tn.isNotEmpty()) e.tonoNombre = tn; }
            e.portada = meta.getProperty ("portada", "").toString();

            status ("Preparando " + juce::String (i + 1) + "/" + juce::String (cs->size()) + ": " + e.titulo);

            const auto pistasUrl = serverUrl + "/api/live/pistas/" + juce::String (e.id) + "?t=" + juce::String (e.tono);
            auto pv = juce::JSON::parse (httpGet (pistasUrl, token));

            // Si el tono no está renderizado en el servidor, pedir que se genere y esperar
            if (e.tono != 0 && ! (bool) pv.getProperty ("listo", false))
            {
                status (juce::String::fromUTF8 ("Este tono no se encontraba renderizado, espere unos momentos mientras se renderiza\xe2\x80\xa6"));
                httpPostForm (serverUrl + "/api/live/render/" + juce::String (e.id) + "/"
                              + juce::String (e.tono), {}, token);
                for (int tries = 0; tries < 600 && ! threadShouldExit(); ++tries)   // hasta ~10 min
                {
                    auto est = juce::JSON::parse (httpGet (serverUrl + "/api/live/render/" + juce::String (e.id) + "/"
                                                           + juce::String (e.tono) + "/estado", token));
                    if ((bool) est.getProperty ("listo", false)) break;
                    auto prog = est.getProperty ("progreso", "").toString();
                    status (juce::String::fromUTF8 ("Renderizando tono de ") + e.titulo
                            + (prog.isNotEmpty() ? ("   " + prog) : juce::String()));
                    juce::Thread::sleep (1000);
                }
                pv = juce::JSON::parse (httpGet (pistasUrl, token));   // re-pedir, ya con el tono listo
            }

            e.tempo = (double) pv.getProperty ("tempo", 0.0);
            auto comp = pv.getProperty ("compas", "4/4").toString();
            e.compas = comp.isNotEmpty() ? comp : juce::String ("4/4");
            e.beatsPerBar = comp.upToFirstOccurrenceOf ("/", false, false).getIntValue();
            if (e.beatsPerBar < 1) e.beatsPerBar = 4;

            if (auto* secs = pv.getProperty ("secciones", juce::var()).getArray())
                for (auto& sc : *secs)
                {
                    e.secTimes.add ((double) sc.getProperty ("t", 0.0));
                    e.secNames.add (sc.getProperty ("nombre", sc.getProperty ("tipo", juce::var (""))).toString());
                }

            e.folder = cacheDir.getChildFile ("song_" + juce::String (e.id) + "_t" + juce::String (e.tono));
            e.folder.createDirectory();
            if (e.portada.isNotEmpty())
            {
                auto cov = e.folder.getChildFile ("cover.jpg");
                if (! cov.existsAsFile() || cov.getSize() < 500)
                    httpDownload (serverUrl + "/static/" + e.portada, token, cov);
                e.coverFile = cov;
            }

            if (auto* stems = pv.getProperty ("stems", juce::var()).getArray())
                for (auto& st : *stems)
                {
                    auto fn = st.getProperty ("file", "").toString();
                    if (fn.isEmpty()) continue;
                    e.famFiles.add (fn);
                    e.famNames.add (st.getProperty ("familia", "").toString());
                }

            // Cajas MIDI + notas del servidor
            {
                auto mv = juce::JSON::parse (httpGet (serverUrl + "/api/live/midi/" + juce::String (e.id), token));
                if (auto* cs = mv.getProperty ("cajas", juce::var()).getArray())
                    for (auto& cv : *cs)
                    {
                        MidiBox mb;
                        mb.id     = cv.getProperty ("id", "").toString();
                        mb.nombre = cv.getProperty ("nombre", "").toString();
                        mb.canal  = (int) cv.getProperty ("canal", 1);
                        if (auto* ns = cv.getProperty ("notas", juce::var()).getArray())
                            for (auto& nv : *ns)
                            {
                                MidiNoteEv mn;
                                mn.seg  = (double) nv.getProperty ("seg", 0.0);
                                mn.note = (int) nv.getProperty ("note", 0);
                                mn.vel  = (int) nv.getProperty ("vel", 100);
                                mb.notas.add (mn);
                            }
                        e.midiBoxes.add (mb);
                    }
            }
            {   // grilla de negras para el MIDI clock (sigue cambios de tempo / medleys)
                auto bg = juce::JSON::parse (httpGet (serverUrl + "/api/live/beatgrid/" + juce::String (e.id), token));
                if (auto* arr = bg.getProperty ("grid", juce::var()).getArray())
                    for (auto& x : *arr) e.beatGrid.add ((double) x);
            }
            out.add (e);
        }

        // FASE A lista: metadata + portadas -> ya se pueden mostrar las tarjetas
        if (onMeta) { auto cb = onMeta; juce::MessageManager::callAsync ([cb, out] { cb (out); }); }

        // FASE B: descargar el audio de cada canción, reportando el % por tarjeta
        for (int i = 0; i < out.size() && ! threadShouldExit(); ++i)
        {
            auto& e = out.getReference (i);
            const int N = juce::jmax (1, e.famFiles.size());
            for (int k = 0; k < e.famFiles.size(); ++k)
            {
                if (threadShouldExit()) return;
                const auto fn = e.famFiles[k];
                auto dest = e.folder.getChildFile (fn);
                if (! dest.existsAsFile() || dest.getSize() < 2000)
                {
                    const auto durl = serverUrl + "/api/live/pista/" + juce::String (e.id) + "/"
                                      + juce::URL::addEscapeChars (fn, false) + "?t=" + juce::String (e.tono);
                    const int kk = k, ii = i;
                    bool ok = false;
                    for (int intento = 0; intento < 3 && ! ok && ! threadShouldExit(); ++intento)
                        ok = httpDownload (durl, token, dest,
                                [this, ii, kk, N] (double p) { progress (ii, (kk + p) / (double) N); },
                                [this] { return threadShouldExit(); });
                }
                progress (i, (double) (k + 1) / (double) N);
            }
            progress (i, 1.0);   // canción i lista para tocar
        }

        status ("Repertorio listo: " + slName);
        if (onDone) { auto cb = onDone; juce::MessageManager::callAsync ([cb, out] { cb (out); }); }
    }

    juce::String serverUrl, token;
    juce::String wantedId;     // setlist elegido (vacio = el primero)
    juce::String resolvedId;   // id real del setlist cargado (lo llena run())
    juce::String resolvedName; // nombre del setlist cargado
    juce::File cacheDir;
};

static juce::Colour cajaColour (int i)
{
    static const juce::uint32 c[8] = { 0xffE6C15A, 0xff4CC1FF, 0xff4C7CFF, 0xffB07CFF,
                                       0xffFF6BA0, 0xff5CD98A, 0xffFF8A4C, 0xff9AD84C };
    return juce::Colour (c[juce::jlimit (0, 7, i)]);
}

struct MidiPanel : public juce::Component
{
    struct Row
    {
        juce::String cajaId;
        juce::Label name;
        juce::ComboBox port, chan;
        juce::ToggleButton on;
        juce::Rectangle<int> swatch;
        bool noChan = false;               // fila especial (MIDI Clock): sin selector de canal
    };
    juce::OwnedArray<Row> rows;
    juce::File cfgFile;
    std::function<void()> onChanged;

    MidiPanel()
    {
        static const char* nm[7]  = { "Lyrics","Lights 1","Lights 2","Patches 1","Patches 2","Guitar","Aux 1" };
        static const char* ids[7] = { "lyrics","lights1","lights2","patches1","patches2","guitar","aux1" };
        static const int   chd[7] = { 16,1,2,3,4,5,6 };
        for (int i = 0; i < 7; ++i)
        {
            auto* r = rows.add (new Row());
            r->cajaId = ids[i];
            r->name.setText (nm[i], juce::dontSendNotification);
            r->name.setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
            r->name.setFont (juce::Font (14.5f, juce::Font::bold));
            addAndMakeVisible (r->name);
            for (auto* cb : { &r->port, &r->chan })
            {
                cb->setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1f1f1f));
                cb->setColour (juce::ComboBox::textColourId, juce::Colour (0xfff2f2f2));
                cb->setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff2a2a2a));
                cb->setColour (juce::ComboBox::arrowColourId, juce::Colour (0xffa3a3a3));
            }
            for (int c = 1; c <= 16; ++c) r->chan.addItem ("Canal " + juce::String (c), c);
            r->chan.setSelectedId (chd[i], juce::dontSendNotification);
            r->chan.onChange = [this] { saveCfg(); };
            addAndMakeVisible (r->chan);
            r->port.onChange = [this] { saveCfg(); };
            addAndMakeVisible (r->port);
            r->on.onClick = [this] { saveCfg(); };
            addAndMakeVisible (r->on);
        }
        {   // fila especial: MIDI Clock (solo salida, sin canal)
            auto* r = rows.add (new Row());
            r->cajaId = "clock"; r->noChan = true;
            r->name.setText ("MIDI Clock", juce::dontSendNotification);
            r->name.setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
            r->name.setFont (juce::Font (14.5f, juce::Font::bold));
            addAndMakeVisible (r->name);
            r->port.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1f1f1f));
            r->port.setColour (juce::ComboBox::textColourId, juce::Colour (0xfff2f2f2));
            r->port.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff2a2a2a));
            r->port.setColour (juce::ComboBox::arrowColourId, juce::Colour (0xffa3a3a3));
            r->port.onChange = [this] { saveCfg(); };
            addAndMakeVisible (r->port);
            r->on.onClick = [this] { saveCfg(); };
            addAndMakeVisible (r->on);
        }
        refreshPorts();
    }

    void refreshPorts()
    {
        auto devs = juce::MidiOutput::getAvailableDevices();
        for (auto* r : rows)
        {
            const auto prev = r->port.getText();
            r->port.clear (juce::dontSendNotification);
            r->port.addItem (juce::String::fromUTF8 ("\xe2\x80\x94 sin salida \xe2\x80\x94"), 1);
            for (int i = 0; i < devs.size(); ++i) r->port.addItem (devs[i].name, i + 2);
            int sel = 1;
            for (int i = 0; i < r->port.getNumItems(); ++i)
                if (r->port.getItemText (i) == prev) { sel = r->port.getItemId (i); break; }
            r->port.setSelectedId (sel, juce::dontSendNotification);
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto fp = getLocalBounds().toFloat();
        juce::ColourGradient grad (juce::Colour (0x16ffffff), fp.getX(), fp.getY(),
                                   juce::Colour (0x05ffffff), fp.getX(), fp.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (fp, 12.0f);
        g.setColour (juce::Colour (0x22ffffff));
        g.drawRoundedRectangle (fp, 12.0f, 1.0f);
        for (int i = 1; i < rows.size(); ++i)
        {
            const float y = (float) rows[i]->name.getY() - 5.0f;
            g.setColour (juce::Colour (0x12ffffff));
            g.drawLine (fp.getX() + 18.0f, y, fp.getRight() - 18.0f, y, 1.0f);
        }
        for (int i = 0; i < rows.size(); ++i)
        {
            auto s = rows[i]->swatch.toFloat();
            g.setColour (rows[i]->noChan ? juce::Colour (0xffC9A96E) : cajaColour (i));
            g.fillEllipse (s.getCentreX() - 5.5f, s.getCentreY() - 5.5f, 11.0f, 11.0f);
        }
    }

    void resized() override
    {
        auto a = getLocalBounds().reduced (18, 14);
        const int rh = juce::jmax (30, a.getHeight() / juce::jmax (1, rows.size()));
        for (auto* r : rows)
        {
            auto row = a.removeFromTop (rh).withSizeKeepingCentre (a.getWidth(), 32);
            r->swatch = row.removeFromLeft (24);
            r->name.setBounds (row.removeFromLeft (140));
            r->on.setBounds   (row.removeFromRight (54));
            row.removeFromRight (12);
            if (! r->noChan)
            {
                r->chan.setBounds (row.removeFromRight (120));
                row.removeFromRight (12);
            }
            r->port.setBounds (row);
        }
    }

    bool isOn (int i) const { return i >= 0 && i < rows.size() && rows[i]->on.getToggleState(); }
    int  channel (int i) const { return (i >= 0 && i < rows.size()) ? rows[i]->chan.getSelectedId() : 1; }
    juce::String portName (int i) const { return (i >= 0 && i < rows.size() && rows[i]->port.getSelectedId() > 1) ? rows[i]->port.getText() : juce::String(); }
    int count() const { return rows.size(); }
    juce::String clockPortName() const { for (auto* r : rows) if (r->cajaId == "clock") return (r->port.getSelectedId() > 1 ? r->port.getText() : juce::String()); return {}; }
    bool clockOn() const { for (auto* r : rows) if (r->cajaId == "clock") return r->on.getToggleState(); return false; }

    void saveCfg()
    {
        juce::Array<juce::var> a;
        for (auto* r : rows)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("id", r->cajaId);
            o->setProperty ("port", r->port.getSelectedId() > 1 ? r->port.getText() : juce::String());
            o->setProperty ("chan", r->chan.getSelectedId());
            o->setProperty ("on", r->on.getToggleState());
            a.add (juce::var (o));
        }
        if (cfgFile != juce::File())
            cfgFile.replaceWithText (juce::JSON::toString (juce::var (a)));
        if (onChanged) onChanged();
    }

    void loadCfg (juce::File f)
    {
        cfgFile = f;
        auto v = juce::JSON::parse (f);
        if (auto* a = v.getArray())
            for (auto& e : *a)
            {
                const auto id = e.getProperty ("id", "").toString();
                for (auto* r : rows) if (r->cajaId == id)
                {
                    r->chan.setSelectedId (juce::jlimit (1, 16, (int) e.getProperty ("chan", r->chan.getSelectedId())), juce::dontSendNotification);
                    r->on.setToggleState ((bool) e.getProperty ("on", false), juce::dontSendNotification);
                    const auto pn = e.getProperty ("port", "").toString();
                    if (pn.isNotEmpty())
                        for (int i = 0; i < r->port.getNumItems(); ++i)
                            if (r->port.getItemText (i) == pn) { r->port.setSelectedId (r->port.getItemId (i), juce::dontSendNotification); break; }
                }
            }
    }
};

struct RepertoirePicker : public juce::Component
{
    struct Item { juce::String id, nombre, fecha; int nCanciones = 0; bool cached = false; int dlPct = -1; };
    juce::Array<Item> items;
    int selected = -1;
    int menuRow = -1;                 // fila con el menú (Cargar/Guardar/Borrar) desplegado
    bool loading = true;
    bool dirty = false;               // hay cambios de mezcla sin guardar
    juce::String currentLoadedId;     // repertorio cargado (para mostrar "Guardar")
    juce::uint32 savedAt = 0;
    std::function<void (juce::String)> onLoad;
    std::function<void (juce::String)> onSave;
    std::function<void (juce::String)> onDelete;
    std::function<void (juce::String)> onDownload;
    std::function<void()> onNew;
    juce::TextButton newBtn, loadBtn, closeBtn;

    RepertoirePicker()
    {
        newBtn.setButtonText (juce::String::fromUTF8 ("+ Nuevo"));
        newBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        newBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff7Cc6ff));
        newBtn.onClick = [this] { setVisible (false); if (onNew) onNew(); };
        addAndMakeVisible (newBtn);
        loadBtn.setButtonText ("Cargar");
        loadBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffffffff));
        loadBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff0a0a0a));
        loadBtn.onClick = [this]
        {
            if (selected >= 0 && selected < items.size() && onLoad)
            { auto id = items[selected].id; setVisible (false); onLoad (id); }
        };
        addAndMakeVisible (loadBtn);
        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        closeBtn.onClick = [this] { setVisible (false); };
        addAndMakeVisible (closeBtn);
        setAlwaysOnTop (true);
    }

    juce::Rectangle<int> panelBounds() const
    {
        const int w = 440;
        const int h = 150 + juce::jmax (1, items.size()) * 54;
        return getLocalBounds().withSizeKeepingCentre (w, juce::jmin (h, getHeight() - 80));
    }
    juce::Rectangle<int> rowBounds (int i) const
    {
        auto p = panelBounds();
        return { p.getX() + 20, p.getY() + 64 + i * 54, p.getWidth() - 40, 48 };
    }
    juce::Rectangle<int> chipRect (int i) const
    {
        auto r = rowBounds (i);
        return { r.getRight() - 108, r.getCentreY() - 13, 96, 26 };
    }
    bool canSave (int i) const
    {
        return i >= 0 && i < items.size() && currentLoadedId.isNotEmpty() && items[i].id == currentLoadedId;
    }
    juce::Array<juce::Rectangle<int>> belowRects (int i) const   // Guardar/Borrar (pequeños) debajo de la fila
    {
        juce::Array<juce::Rectangle<int>> out;
        auto r = rowBounds (i);
        const int bw = 84, bh = 24, gap = 8;
        int x = r.getX() + 4, y = r.getBottom() + 4;
        const int n = canSave (i) ? 2 : 1;   // [Guardar?] Borrar  (Descargar va en el chip de la derecha)
        for (int k = 0; k < n; ++k) { out.add ({ x, y, bw, bh }); x += bw + gap; }
        return out;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xC0000000));
        auto p = panelBounds().toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (p, 14.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (p, 14.0f, 1.2f);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (17.0f, juce::Font::bold));
        g.drawText ("Repertorio", panelBounds().removeFromTop (56).reduced (22, 0),
                    juce::Justification::centredLeft);
        if (juce::Time::getMillisecondCounter() - savedAt < 1600)
        {
            g.setColour (juce::Colour (0xff3ED66E));
            g.setFont (juce::Font (12.5f, juce::Font::bold));
            g.drawText (juce::String::fromUTF8 ("\xe2\x9c\x93 Mezcla guardada"),
                        panelBounds().removeFromTop (56).reduced (22, 0), juce::Justification::centredRight);
        }
        else if (dirty)
        {
            auto hr = panelBounds().removeFromTop (56).reduced (22, 0).withTrimmedRight (40);
            g.setColour (juce::Colour (0xffE5484D));
            g.fillEllipse ((float) hr.getRight() - 10.0f, (float) hr.getCentreY() - 4.0f, 8.0f, 8.0f);
            g.setFont (juce::Font (12.5f, juce::Font::bold));
            g.drawText (juce::String::fromUTF8 ("Cambios sin guardar"), hr.withTrimmedRight (16),
                        juce::Justification::centredRight);
        }

        if (loading)
        {
            g.setColour (juce::Colour (0xffa3a3a3)); g.setFont (juce::Font (13.0f));
            g.drawText ("Buscando repertorios...", panelBounds().reduced (20), juce::Justification::centred);
        }
        else if (items.isEmpty())
        {
            g.setColour (juce::Colour (0xffa3a3a3)); g.setFont (juce::Font (13.0f));
            g.drawText ("No hay repertorios disponibles", panelBounds().reduced (20), juce::Justification::centred);
        }
        for (int i = 0; i < items.size(); ++i)
        {
            auto r = rowBounds (i).toFloat();
            const bool sel = (i == selected);
            g.setColour (sel ? juce::Colour (0x2Cffffff) : juce::Colour (0xff1c1c1c));
            g.fillRoundedRectangle (r, 9.0f);
            g.setColour (sel ? juce::Colours::white : juce::Colour (0x22ffffff));
            g.drawRoundedRectangle (r, 9.0f, sel ? 1.6f : 1.0f);
            auto txt = r.reduced (14, 6).withTrimmedRight (116.0f);
            g.setColour (juce::Colours::white); g.setFont (juce::Font (14.0f, juce::Font::bold));
            g.drawText (items[i].nombre, txt.removeFromTop (18.0f), juce::Justification::centredLeft);
            g.setColour (juce::Colour (0xffa3a3a3)); g.setFont (juce::Font (11.5f));
            g.drawText (items[i].fecha + "   \xc2\xb7   " + juce::String (items[i].nCanciones) + " canciones",
                        txt.removeFromBottom (16.0f), juce::Justification::centredLeft);
            // chip de estado de descarga (derecha)
            auto ch = chipRect (i).toFloat();
            juce::Colour cbg, cfg; juce::String ctxt;
            if (items[i].dlPct >= 0)        { cbg = juce::Colour (0xff3a2f10); cfg = juce::Colour (0xffE6C15A); ctxt = juce::String (items[i].dlPct) + "%"; }
            else if (items[i].cached)       { cbg = juce::Colour (0xff14301a); cfg = juce::Colour (0xff8fe0a0); ctxt = juce::String::fromUTF8 ("\xe2\x9c\x93 Offline"); }
            else                            { cbg = juce::Colour (0xff12203c); cfg = juce::Colour (0xff7Cc6ff); ctxt = juce::String::fromUTF8 ("\xe2\xa4\x93 Descargar"); }
            g.setColour (cbg); g.fillRoundedRectangle (ch, 7.0f);
            g.setColour (cfg); g.setFont (juce::Font (11.5f, juce::Font::bold));
            g.drawText (ctxt, ch, juce::Justification::centred);
        }
        // Menú de acciones sobre la fila tocada
        if (menuRow >= 0 && menuRow < items.size())
        {
            const bool save = canSave (menuRow);
            auto drawBtn = [&g] (juce::Rectangle<int> rb, juce::Colour bg, juce::Colour fg, const juce::String& t)
            {
                g.setColour (bg); g.fillRoundedRectangle (rb.toFloat(), 6.0f);
                g.setColour (fg); g.setFont (juce::Font (11.5f, juce::Font::bold));
                g.drawText (t, rb, juce::Justification::centred);
            };
            auto br = belowRects (menuRow);
            int k = 0;
            if (save) drawBtn (br[k++], juce::Colour (0xff17361f), juce::Colour (0xff5CD98A), "Guardar");
            drawBtn (br[k], juce::Colour (0xff2a1414), juce::Colour (0xffe05555), "Borrar");
        }
    }

    void resized() override { layoutButtons(); }
    void layoutButtons()
    {
        auto p = panelBounds();
        closeBtn.setBounds (p.getRight() - 46, p.getY() + 12, 34, 30);
        auto brow = juce::Rectangle<int> (p.getX() + 20, p.getBottom() - 54, p.getWidth() - 40, 36);
        newBtn.setBounds (brow.removeFromLeft (110));
        loadBtn.setBounds (brow.removeFromRight (120));
    }

    void scheduleFlash()
    {
        juce::Component::SafePointer<RepertoirePicker> sp (this);
        juce::Timer::callAfterDelay (1650, [sp] { if (sp) sp->repaint(); });
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (menuRow >= 0 && menuRow < items.size())   // menú abierto: primero sus botones
        {
            const bool save = canSave (menuRow);
            const auto id = items[menuRow].id;
            auto br = belowRects (menuRow);
            int k = 0;
            if (save && br[k++].contains (e.getPosition())) { savedAt = juce::Time::getMillisecondCounter(); if (onSave) onSave (id); menuRow = -1; scheduleFlash(); repaint(); return; }
            if (k < br.size() && br[k].contains (e.getPosition())) { menuRow = -1; if (onDelete) onDelete (id); return; }
        }
        for (int i = 0; i < items.size(); ++i)   // chip de descarga a la derecha de la fila
            if (chipRect (i).contains (e.getPosition()))
            { if (items[i].dlPct < 0 && ! items[i].cached && onDownload) onDownload (items[i].id); return; }
        for (int i = 0; i < items.size(); ++i)   // tocar una fila la selecciona y abre su menú
            if (rowBounds (i).contains (e.getPosition())) { selected = i; menuRow = i; repaint(); return; }
        if (menuRow >= 0) { menuRow = -1; repaint(); }          // clic fuera de filas: cerrar menú
        if (! panelBounds().contains (e.getPosition())) setVisible (false);
    }

    void setItems (juce::Array<Item> it)
    {
        items = std::move (it);
        loading = false;
        menuRow = -1;
        if (selected < 0 && ! items.isEmpty()) selected = 0;
        layoutButtons();
        repaint();
    }
};

struct StoragePanel : public juce::Component
{
    juce::TextButton freeBtn, autoBtn, capMinus, capPlus, closeBtn;
    juce::int64 total = 0, unused = 0;
    bool autoClean = false;
    int capGB = 0;                 // 0 = sin límite
    juce::Rectangle<int> capLblBounds, capValBounds, autoHdrBounds, autoDescBounds;
    std::function<void()> onFreeUnused;
    std::function<void (bool)> onAutoClean;
    std::function<void (int)> onCap;

    StoragePanel()
    {
        auto st = [] (juce::TextButton& b, juce::uint32 bg, juce::uint32 tx)
        { b.setColour (juce::TextButton::buttonColourId, juce::Colour (bg));
          b.setColour (juce::TextButton::textColourOffId, juce::Colour (tx)); };
        st (freeBtn, 0xff2a2418, 0xffC9A96E);
        freeBtn.onClick = [this] { if (onFreeUnused) onFreeUnused(); };
        addAndMakeVisible (freeBtn);
        st (autoBtn, 0xff1f1f1f, 0xfff2f2f2);
        autoBtn.onClick = [this] { if (onAutoClean) onAutoClean (! autoClean); };
        addAndMakeVisible (autoBtn);
        st (capMinus, 0xff1f1f1f, 0xfff2f2f2); capMinus.setButtonText ("-");
        capMinus.onClick = [this] { if (onCap) onCap (juce::jmax (0, capGB - 5)); };
        addAndMakeVisible (capMinus);
        st (capPlus, 0xff1f1f1f, 0xfff2f2f2); capPlus.setButtonText ("+");
        capPlus.onClick = [this] { if (onCap) onCap (juce::jmin (500, capGB + 5)); };
        addAndMakeVisible (capPlus);
        st (closeBtn, 0xff1f1f1f, 0xfff2f2f2); closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.onClick = [this] { setVisible (false); };
        addAndMakeVisible (closeBtn);
        setAlwaysOnTop (true);
        refresh();
    }

    void setStats (juce::int64 t, juce::int64 u, bool aut, int cap)
    { total = t; unused = u; autoClean = aut; capGB = cap; refresh(); }

    void refresh()
    {
        freeBtn.setButtonText (juce::String::fromUTF8 ("Liberar sin usar  (") + npFmtBytes (unused) + ")");
        autoBtn.setButtonText (autoClean ? juce::String::fromUTF8 ("Activada") : juce::String::fromUTF8 ("Desactivada"));
        autoBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (autoClean ? 0xff17361f : 0xff1f1f1f));
        autoBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (autoClean ? 0xff8fe0a0 : 0xfff2f2f2));
        freeBtn.setEnabled (unused > 0);
        repaint();
    }

    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withSizeKeepingCentre (440, 446); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xC0000000));
        auto pf = panelBounds().toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (pf, 14.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (pf, 14.0f, 1.2f);
        auto in = panelBounds().reduced (24, 0);
        g.setColour (juce::Colours::white); g.setFont (juce::Font (17.0f, juce::Font::bold));
        g.drawText (juce::String::fromUTF8 ("Almacenamiento"), in.removeFromTop (52), juce::Justification::centredLeft);
        g.setFont (15.0f); g.setColour (juce::Colour (0xffe8e8e8));
        g.drawText (juce::String::fromUTF8 ("Total en cach\xc3\xa9:   ") + npFmtBytes (total), in.removeFromTop (26), juce::Justification::centredLeft);
        g.setFont (13.5f); g.setColour (juce::Colour (0xffa3a3a3));
        g.drawText (juce::String::fromUTF8 ("En uso por tus repertorios:   ") + npFmtBytes (total - unused), in.removeFromTop (22), juce::Justification::centredLeft);
        g.drawText (juce::String::fromUTF8 ("Sin usar (se puede liberar):   ") + npFmtBytes (unused), in.removeFromTop (22), juce::Justification::centredLeft);

        // ── sección: Limpieza automática (agrupa el interruptor + el límite) ──
        g.setColour (juce::Colour (0x18ffffff));
        g.fillRect (juce::Rectangle<int> (in.getX(), autoHdrBounds.getY() - 8, in.getWidth(), 1));
        g.setColour (juce::Colour (0xffC9A96E)); g.setFont (juce::Font (12.0f, juce::Font::bold));
        g.drawText (juce::String::fromUTF8 ("LIMPIEZA AUTOM\xc3\x81TICA"), autoHdrBounds, juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xff8a8a8a)); g.setFont (12.0f);
        g.drawText (juce::String::fromUTF8 ("Borra solo el audio que no est\xc3\xa1 en ninguno de tus repertorios."),
                    autoDescBounds, juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xfff2f2f2)); g.setFont (13.5f);
        g.drawText (juce::String::fromUTF8 ("L\xc3\xadmite de cach\xc3\xa9"), capLblBounds, juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xffe8e8e8)); g.setFont (juce::Font (14.0f, juce::Font::bold));
        g.drawText (capGB > 0 ? juce::String (capGB) + " GB" : juce::String::fromUTF8 ("sin l\xc3\xadmite"),
                    capValBounds, juce::Justification::centred);
    }

    void resized() override
    {
        auto p = panelBounds();
        closeBtn.setBounds (p.getRight() - 46, p.getY() + 12, 34, 30);
        auto b = p.reduced (24);
        b.removeFromTop (52 + 26 + 22 + 22 + 16);      // título + stats
        const int bh = 44;
        freeBtn.setBounds  (b.removeFromTop (bh)); b.removeFromTop (20);
        autoHdrBounds  = b.removeFromTop (18); b.removeFromTop (2);
        autoDescBounds = b.removeFromTop (18); b.removeFromTop (6);
        // fila: [descripción del toggle | botón Activada/Desactivada]
        {
            auto row = b.removeFromTop (bh);
            autoBtn.setBounds (row.removeFromRight (150));
        }
        b.removeFromTop (12);
        // fila del límite: etiqueta ... [-] valor [+]
        auto cr = b.removeFromTop (bh);
        capPlus.setBounds  (cr.removeFromRight (44)); cr.removeFromRight (8);
        capValBounds = cr.removeFromRight (78);       cr.removeFromRight (8);
        capMinus.setBounds (cr.removeFromRight (44)); cr.removeFromRight (12);
        capLblBounds = cr;
    }

    void mouseDown (const juce::MouseEvent& e) override
    { if (! panelBounds().contains (e.getPosition())) setVisible (false); }
};

// LookAndFeel para filas tipo interruptor (nombre a la izquierda, switch a la derecha)
struct SwitchLNF : public juce::LookAndFeel_V4
{
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat();
        g.setColour (juce::Colour (down ? 0xff2a2a2a : (over ? 0xff242424 : 0xff1f1f1f)));
        g.fillRoundedRectangle (r, 10.0f);
        const bool on = b.getToggleState();
        const float sw = 44.0f, sh = 26.0f, pad = 16.0f;
        juce::Rectangle<float> tr (r.getRight() - sw - pad, r.getCentreY() - sh * 0.5f, sw, sh);
        g.setColour (on ? juce::Colour (0xff2FBF5B) : juce::Colour (0xff4a4a4a));
        g.fillRoundedRectangle (tr, sh * 0.5f);
        const float kd = sh - 6.0f;
        const float kx = on ? (tr.getRight() - kd - 3.0f) : (tr.getX() + 3.0f);
        juce::Rectangle<float> knob (kx, tr.getCentreY() - kd * 0.5f, kd, kd);
        g.setColour (juce::Colours::white);
        g.fillEllipse (knob);
    }
    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setColour (juce::Colour (0xfff2f2f2));
        g.setFont (juce::Font (14.5f, juce::Font::bold));
        auto r = b.getLocalBounds().reduced (18, 0);
        r.removeFromRight (44 + 16 + 10);   // deja el hueco del switch
        g.drawText (b.getButtonText(), r, juce::Justification::centredLeft, true);
    }
};

struct SettingsPanel : public juce::Component
{
    juce::TextButton syncBtn, cfgBtn, refreshBtn, storeBtn, countInBtn, masterPSBtn, mixPSBtn, closeBtn;
    SwitchLNF switchLnf;
    ~SettingsPanel() override
    {
        for (auto* b : { &syncBtn, &countInBtn, &masterPSBtn, &mixPSBtn })
            b->setLookAndFeel (nullptr);
    }
    bool syncOn = false, linked = false, countInOn = false, masterPSOn = true, mixPSOn = true;
    juce::Rectangle<int> statusBounds;
    std::function<void (bool)> onSync;
    std::function<void()> onConfig;
    std::function<void()> onRefresh;
    std::function<void()> onStorage;
    std::function<void (bool)> onCountIn;
    std::function<void (bool)> onMasterPS;
    std::function<void (bool)> onMixPS;

    SettingsPanel()
    {
        syncBtn.onClick  = [this] { if (onSync) onSync (! syncOn); };
        addAndMakeVisible (syncBtn);

        cfgBtn.setButtonText (juce::String::fromUTF8 ("Salidas de Audio"));
        cfgBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        cfgBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        cfgBtn.onClick = [this] { if (onConfig) onConfig(); };
        addAndMakeVisible (cfgBtn);

        refreshBtn.setButtonText (juce::String::fromUTF8 ("Actualizar"));
        refreshBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        refreshBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        refreshBtn.onClick = [this] { if (onRefresh) onRefresh(); };
        addAndMakeVisible (refreshBtn);

        storeBtn.setButtonText (juce::String::fromUTF8 ("Almacenamiento"));
        storeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        storeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        storeBtn.onClick = [this] { if (onStorage) onStorage(); };
        addAndMakeVisible (storeBtn);

        countInBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        countInBtn.onClick = [this] { countInOn = ! countInOn; if (onCountIn) onCountIn (countInOn); refresh(); };
        addAndMakeVisible (countInBtn);

        masterPSBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        masterPSBtn.onClick = [this] { masterPSOn = ! masterPSOn; if (onMasterPS) onMasterPS (masterPSOn); refresh(); };
        addAndMakeVisible (masterPSBtn);

        mixPSBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        mixPSBtn.onClick = [this] { mixPSOn = ! mixPSOn; if (onMixPS) onMixPS (mixPSOn); refresh(); };
        addAndMakeVisible (mixPSBtn);

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        closeBtn.onClick = [this] { setVisible (false); };
        addAndMakeVisible (closeBtn);

        for (auto* b : { &syncBtn, &countInBtn, &masterPSBtn, &mixPSBtn }) b->setLookAndFeel (&switchLnf);

        setAlwaysOnTop (true);
        refresh();
    }

    void setState (bool on, bool lk) { syncOn = on; linked = lk; refresh(); }
    void setCountIn (bool on) { countInOn = on; refresh(); }
    void setMasterPS (bool on) { masterPSOn = on; refresh(); }
    void setMixPS (bool on) { mixPSOn = on; refresh(); }

    void refresh()
    {
        syncBtn.setButtonText ("NeuralSync");
        syncBtn.setToggleState (syncOn, juce::dontSendNotification);

        countInBtn.setButtonText (juce::String::fromUTF8 ("Pre-roll por secci\xc3\xb3n"));
        countInBtn.setToggleState (countInOn, juce::dontSendNotification);

        masterPSBtn.setButtonText (juce::String::fromUTF8 ("Master por canci\xc3\xb3n"));
        masterPSBtn.setToggleState (masterPSOn, juce::dontSendNotification);

        mixPSBtn.setButtonText (juce::String::fromUTF8 ("Buses y mute por canci\xc3\xb3n"));
        mixPSBtn.setToggleState (mixPSOn, juce::dontSendNotification);
        repaint();
    }

    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withSizeKeepingCentre (380, 548); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xC0000000));
        auto p = panelBounds().toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (p, 14.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (p, 14.0f, 1.2f);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (17.0f, juce::Font::bold));
        g.drawText (juce::String::fromUTF8 ("Men\xc3\xba"), panelBounds().removeFromTop (56).reduced (22, 0),
                    juce::Justification::centredLeft);

        // Estado del puente: pelotita + texto, centrados en su propia línea
        {
            const juce::String txt = linked ? "enlazado" : (syncOn ? "esperando..." : "sin enlazar");
            juce::GlyphArrangement ga; ga.addLineOfText (juce::Font (11.5f), txt, 0.0f, 0.0f);
            const float tw = ga.getBoundingBox (0, -1, true).getWidth();
            const float dotD = 10.0f, sp = 7.0f, total = dotD + sp + tw;
            const float sx = (float) statusBounds.getCentreX() - total * 0.5f;
            const float cy = (float) statusBounds.getCentreY();
            juce::Rectangle<float> dot (sx, cy - dotD * 0.5f, dotD, dotD);
            g.setColour (linked ? juce::Colour (0xff3ED66E) : juce::Colour (0xff3a3a3a));
            g.fillEllipse (dot);
            if (linked)
            {
                g.setColour (juce::Colour (0xff0a0a0a));
                g.setFont (juce::Font (8.0f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("\xe2\x9c\x93"), dot, juce::Justification::centred);
            }
            g.setColour (juce::Colour (0xffa3a3a3));
            g.setFont (juce::Font (11.5f));
            g.drawText (txt, juce::Rectangle<float> (sx + dotD + sp, cy - 9.0f, tw + 6.0f, 18.0f),
                        juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto p = panelBounds();
        closeBtn.setBounds (p.getRight() - 46, p.getY() + 12, 34, 30);
        auto b = p.reduced (24); b.removeFromTop (44);
        const int bh = 46, gap = 12;
        syncBtn.setBounds     (b.removeFromTop (bh)); b.removeFromTop (gap);
        countInBtn.setBounds  (b.removeFromTop (bh)); b.removeFromTop (gap);
        masterPSBtn.setBounds (b.removeFromTop (bh)); b.removeFromTop (gap);
        mixPSBtn.setBounds    (b.removeFromTop (bh)); b.removeFromTop (gap);
        cfgBtn.setBounds      (b.removeFromTop (bh)); b.removeFromTop (gap);
        storeBtn.setBounds    (b.removeFromTop (bh)); b.removeFromTop (gap);
        refreshBtn.setBounds  (b.removeFromTop (bh)); b.removeFromTop (gap);
        statusBounds = b.removeFromTop (22);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelBounds().contains (e.getPosition())) setVisible (false);
    }
};

// ───────── Ventana de Salida de audio (dispositivo + enrutamiento por familia) ─────────
struct AudioConfigPanel : public juce::Component
{
    struct FamRoute { int mode = 2; int ch = 0; };   // 0 off, 1 mono, 2 estéreo · ch base 0-based
    juce::ComboBox deviceBox;
    juce::Label title, devLbl, chInfo;
    juce::OwnedArray<juce::Label> famLabels;
    juce::OwnedArray<juce::ComboBox> routeBoxes;
    juce::TextButton closeBtn, backBtn;
    juce::ComboBox srBox;                  // selector de frecuencia (sample rate)
    juce::Label srLbl;
    juce::Array<double> srList;
    int numChans = 2;
    std::function<void (const juce::String&)> onDevice;
    std::function<void (int, int, int)> onRoute;   // fam, mode, base
    std::function<void (double)> onSampleRate;     // 0 = automático (seguir dispositivo)
    std::function<void()> onBack;                  // volver al Menú

    AudioConfigPanel()
    {
        setAlwaysOnTop (true);
        auto dark = [] (juce::ComboBox& c)
        {
            c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1f1f1f));
            c.setColour (juce::ComboBox::textColourId,       juce::Colour (0xfff2f2f2));
            c.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0x33ffffff));
            c.setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xffa3a3a3));
        };
        addAndMakeVisible (deviceBox); dark (deviceBox);
        deviceBox.onChange = [this] { if (onDevice && deviceBox.getSelectedId() > 0) onDevice (deviceBox.getText()); };

        title.setText ("Salida de audio", juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, juce::Colours::white);
        title.setFont (juce::Font (17.0f, juce::Font::bold));
        addAndMakeVisible (title);
        devLbl.setText ("Interfaz", juce::dontSendNotification);
        devLbl.setColour (juce::Label::textColourId, juce::Colour (0xffa3a3a3));
        devLbl.setFont (juce::Font (12.0f)); addAndMakeVisible (devLbl);
        chInfo.setColour (juce::Label::textColourId, juce::Colour (0xff7Cc6ff));
        chInfo.setFont (juce::Font (12.0f)); addAndMakeVisible (chInfo);

        srLbl.setText (juce::String::fromUTF8 ("Frecuencia"), juce::dontSendNotification);
        srLbl.setColour (juce::Label::textColourId, juce::Colour (0xffa3a3a3));
        srLbl.setFont (juce::Font (12.0f)); addAndMakeVisible (srLbl);
        addAndMakeVisible (srBox); dark (srBox);
        srBox.onChange = [this]
        {
            const int id = srBox.getSelectedId();
            double rate = (id >= 2 && id - 2 < srList.size()) ? srList[id - 2] : 0.0;
            if (onSampleRate) onSampleRate (rate);
        };

        for (int i = 0; i < kNumFam; ++i)
        {
            auto* l = famLabels.add (new juce::Label());
            l->setText (juce::String::fromUTF8 (kRouteFam[i]), juce::dontSendNotification);
            l->setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
            l->setFont (juce::Font (13.0f)); addAndMakeVisible (l);
            auto* c = routeBoxes.add (new juce::ComboBox()); dark (*c);
            const int fi = i;
            c->onChange = [this, fi] { fireRoute (fi); };
            addAndMakeVisible (c);
        }
        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        closeBtn.onClick = [this] { setVisible (false); };
        addAndMakeVisible (closeBtn);

        backBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x80\xb9 Men\xc3\xba"));
        backBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        backBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        backBtn.onClick = [this] { setVisible (false); if (onBack) onBack(); };
        addAndMakeVisible (backBtn);
    }

    void setDevices (const juce::StringArray& names, const juce::String& current)
    {
        deviceBox.clear (juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i) deviceBox.addItem (names[i], i + 1);
        int sel = names.indexOf (current);
        deviceBox.setSelectedId (sel >= 0 ? sel + 1 : (names.isEmpty() ? 0 : 1), juce::dontSendNotification);
    }

    void buildRouteItems (int chans)
    {
        numChans = juce::jlimit (0, 32, chans);
        chInfo.setText (juce::String (numChans) + juce::String::fromUTF8 (" canales disponibles"), juce::dontSendNotification);
        for (auto* c : routeBoxes)
        {
            c->clear (juce::dontSendNotification);
            c->addItem ("Off", 1);
            for (int k = 1; k <= numChans; ++k)          c->addItem (juce::String (k), 100 + k);                                  // mono
            for (int k = 1; k + 1 <= numChans; k += 2)   c->addItem (juce::String (k) + "/" + juce::String (k + 1), 200 + k);     // estéreo
        }
    }

    void setSampleRates (juce::Array<double> avail, double current, double preferred)
    {
        srBox.clear (juce::dontSendNotification);
        srList.clearQuick();
        const juce::String cur = current > 0.0 ? juce::String (current / 1000.0, 1) + " kHz" : juce::String();
        srBox.addItem (juce::String::fromUTF8 ("Autom\xc3\xa1tico") + (cur.isNotEmpty() ? (" (" + cur + ")") : juce::String()), 1);
        int sel = 1, id = 2;
        for (double r : avail)
        {
            if (r < 22000.0) continue;
            srList.add (r);
            srBox.addItem (juce::String (r / 1000.0, 1) + " kHz", id);
            if (preferred > 0.0 && std::abs (preferred - r) < 1.0) sel = id;
            ++id;
        }
        srBox.setSelectedId (sel, juce::dontSendNotification);
    }

    void setRoute (int fam, int mode, int base)
    {
        if (fam < 0 || fam >= routeBoxes.size()) return;
        int id = 1;
        if      (mode == 1) id = 100 + (base + 1);
        else if (mode == 2) id = 200 + (base + 1);
        routeBoxes[fam]->setSelectedId (id, juce::dontSendNotification);
    }

    void fireRoute (int fam)
    {
        if (! onRoute || fam < 0 || fam >= routeBoxes.size()) return;
        const int id = routeBoxes[fam]->getSelectedId();
        int mode = 0, base = 0;
        if      (id >= 200) { mode = 2; base = id - 200 - 1; }
        else if (id >= 100) { mode = 1; base = id - 100 - 1; }
        onRoute (fam, mode, base);
    }

    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withSizeKeepingCentre (480, 624); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xC0000000));
        auto p = panelBounds().toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (p, 14.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (p, 14.0f, 1.2f);
    }

    void resized() override
    {
        auto p = panelBounds();
        closeBtn.setBounds (p.getRight() - 46, p.getY() + 12, 34, 30);
        backBtn.setBounds (p.getX() + 14, p.getY() + 13, 76, 28);
        auto b = p.reduced (22);
        title.setBounds (b.removeFromTop (34).withTrimmedLeft (78));
        b.removeFromTop (4);
        devLbl.setBounds (b.removeFromTop (16));
        deviceBox.setBounds (b.removeFromTop (30));
        chInfo.setBounds (b.removeFromTop (20));
        b.removeFromTop (6);
        srLbl.setBounds (b.removeFromTop (16));
        srBox.setBounds (b.removeFromTop (30));
        b.removeFromTop (8);
        for (int i = 0; i < kNumFam; ++i)
        {
            auto row = b.removeFromTop (32);
            famLabels[i]->setBounds (row.removeFromLeft (160));
            routeBoxes[i]->setBounds (row.reduced (0, 2));
            b.removeFromTop (4);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelBounds().contains (e.getPosition())) setVisible (false);
    }
};

// ───────── Panel de edición de repertorio (biblioteca + grid de tonos) ─────────
struct RepEditPanel : public juce::Component, private juce::Timer
{
    enum Mode { Biblioteca, Tono };
    Mode mode = Biblioteca;
    juce::String serverUrl, token;

    struct BibItem { int id = 0; juce::String titulo, tono, artista, portada; juce::Image cover; };
    juce::Array<BibItem> bib, bibAll;
    int bibScroll = 0;                 // desplazamiento vertical de la biblioteca
    juce::TextEditor searchBox;

    int songId = 0; juce::String songTitle; bool addFlow = false;
    struct Key { juce::String nombre; int sem = 0; bool rendered = false; };
    juce::Array<Key> keys;
    int renderingSem = 99, pendIdx = -1, progHechos = 0, progTotal = 0;

    juce::TextButton closeBtn, backBtn;
    std::function<void (int)> onPickSong;            // biblioteca -> elegir cancion
    std::function<void (int, juce::String)> onChoose; // (semitonos, nombre) -> aplicar

    // #2 punto de inicio/fin por canción
    juce::TextButton inTgl, outTgl;                  // activar inicio / fin
    juce::TextEditor inEdit, outEdit;                // min:seg
    bool inOn = false, outOn = false;
    std::function<void (int, double, double)> onInOut;   // (songId, inicio|-1, fin|-1)

    // Pad Player por canción (intro / outro)
    juce::TextButton padIntroTgl, padOutroTgl;
    bool padIn = false, padOut = false;
    std::function<void (int, bool, bool)> onPadPlayer;   // (songId, intro, outro)
    void pushPad() { if (onPadPlayer) onPadPlayer (songId, padIn, padOut); }

    static juce::String secsToMMSS (double s)
    {
        if (s < 0.0) s = 0.0;
        const int t = (int) (s + 0.5);
        return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
    }
    static double mmssToSecs (const juce::String& txt)
    {
        auto t = txt.trim();
        if (t.isEmpty()) return -1.0;
        if (t.contains (":"))
        {
            const int m = t.upToFirstOccurrenceOf (":", false, false).getIntValue();
            const int s = t.fromLastOccurrenceOf (":", false, false).getIntValue();
            return (double) (m * 60 + s);
        }
        return (double) t.getIntValue();
    }
    void pushInOut()
    {
        if (onInOut) onInOut (songId,
                              inOn  ? juce::jmax (0.0, mmssToSecs (inEdit.getText()))  : -1.0,
                              outOn ? juce::jmax (0.0, mmssToSecs (outEdit.getText())) : -1.0);
    }
    void refreshInOut()   // estado visual de toggles/editores
    {
        auto styleTgl = [] (juce::TextButton& b, bool on)
        {
            b.setButtonText (on ? juce::String::fromUTF8 ("\xe2\x9c\x93") : "");
            b.setColour (juce::TextButton::buttonColourId, on ? juce::Colour (0xff17361f) : juce::Colour (0xff1f1f1f));
            b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff3ED66E));
        };
        styleTgl (inTgl, inOn);
        styleTgl (outTgl, outOn);
        styleTgl (padIntroTgl, padIn);
        styleTgl (padOutroTgl, padOut);
        inEdit.setEnabled (inOn);   outEdit.setEnabled (outOn);
        inEdit.setAlpha (inOn ? 1.0f : 0.4f);   outEdit.setAlpha (outOn ? 1.0f : 0.4f);
        const bool t = (mode == Tono);
        inTgl.setVisible (t);  outTgl.setVisible (t);
        inEdit.setVisible (t); outEdit.setVisible (t);
        padIntroTgl.setVisible (t); padOutroTgl.setVisible (t);
    }

    RepEditPanel()
    {
        setAlwaysOnTop (true);
        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        closeBtn.onClick = [this] { stopTimer(); renderingSem = 99; setVisible (false); };   // cierra del todo
        addAndMakeVisible (closeBtn);

        backBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x80\xb9 Atr\xc3\xa1s"));         // ‹ Atrás
        backBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        backBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        backBtn.onClick = [this] { stopTimer(); renderingSem = 99; showBiblioteca(); };       // vuelve a la lista
        addChildComponent (backBtn);

        searchBox.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Buscar canci\xc3\xb3n\xe2\x80\xa6"), juce::Colour (0xff777777));
        searchBox.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff1c1c1c));
        searchBox.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        searchBox.setColour (juce::TextEditor::outlineColourId, juce::Colour (0x33ffffff));
        searchBox.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0x66ffffff));
        searchBox.onTextChange = [this] { applyFilter(); };
        addChildComponent (searchBox);

        // #2 controles de inicio/fin
        auto setupTgl = [this] (juce::TextButton& b)
        {
            b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
            addChildComponent (b);
        };
        setupTgl (inTgl);  setupTgl (outTgl);
        inTgl.onClick  = [this] { inOn  = ! inOn;  if (inOn  && inEdit.getText().trim().isEmpty())  inEdit.setText ("0:00", false); refreshInOut(); pushInOut(); };
        outTgl.onClick = [this] { outOn = ! outOn; if (outOn && outEdit.getText().trim().isEmpty()) outEdit.setText ("0:00", false); refreshInOut(); pushInOut(); };
        setupTgl (padIntroTgl); setupTgl (padOutroTgl);
        padIntroTgl.onClick = [this] { padIn  = ! padIn;  refreshInOut(); pushPad(); };
        padOutroTgl.onClick = [this] { padOut = ! padOut; refreshInOut(); pushPad(); };

        auto setupEdit = [this] (juce::TextEditor& e)
        {
            e.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff1c1c1c));
            e.setColour (juce::TextEditor::textColourId, juce::Colours::white);
            e.setColour (juce::TextEditor::outlineColourId, juce::Colour (0x33ffffff));
            e.setJustification (juce::Justification::centred);
            e.setInputRestrictions (5, "0123456789:");
            e.onReturnKey = [this] { pushInOut(); };
            e.onFocusLost = [this] { pushInOut(); };
            addChildComponent (e);
        };
        setupEdit (inEdit);  setupEdit (outEdit);
    }

    void applyFilter()
    {
        const auto q = searchBox.getText().trim();
        bib.clearQuick();
        for (auto& b : bibAll) if (q.isEmpty() || b.titulo.containsIgnoreCase (q) || b.artista.containsIgnoreCase (q)) bib.add (b);
        bibScroll = 0;
        repaint();
    }

    std::function<void (juce::Array<BibItem>)> onNeedCovers;   // pedir portadas al MainComponent

    void openBiblioteca (juce::Array<BibItem> items)
    { mode = Biblioteca; bibAll = std::move (items); searchBox.setText ("", false); searchBox.setVisible (true);
      applyFilter(); renderingSem = 99; stopTimer(); resized(); repaint();
      if (onNeedCovers) onNeedCovers (bibAll); }

    void setBibCover (int id, juce::Image img)
    {
        if (! img.isValid()) return;
        for (auto& b : bibAll) if (b.id == id) b.cover = img;
        for (auto& b : bib)    if (b.id == id) b.cover = img;
        repaint();
    }
    void openTono (int id, juce::String title, bool add, juce::Array<Key> ks, double inSec = -1.0, double outSec = -1.0,
                   bool pIntro = false, bool pOutro = false)
    { mode = Tono; songId = id; songTitle = title; addFlow = add; keys = std::move (ks); searchBox.setVisible (false);
      inOn = (inSec >= 0.0); outOn = (outSec >= 0.0);
      padIn = pIntro; padOut = pOutro;
      inEdit.setText (secsToMMSS (inSec >= 0.0 ? inSec : 0.0), false);
      outEdit.setText (secsToMMSS (outSec >= 0.0 ? outSec : 0.0), false);
      refreshInOut();
      renderingSem = 99; stopTimer(); resized(); repaint(); }

    void showBiblioteca()   // volver del grid de tonos a la lista de canciones
    { mode = Biblioteca; searchBox.setVisible (true); refreshInOut(); renderingSem = 99; stopTimer(); resized(); repaint(); }

    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withSizeKeepingCentre (470, 636); }

    juce::Rectangle<int> keyRect (int i) const
    {
        auto p = panelBounds().reduced (22); p.removeFromTop (58);
        const int cols = 4, gap = 9, cw = (p.getWidth() - (cols - 1) * gap) / cols, ch = 46;
        return { p.getX() + (i % cols) * (cw + gap), p.getY() + (i / cols) * (ch + gap), cw, ch };
    }
    juce::Rectangle<int> inOutArea() const   // #2 zona de inicio/fin, debajo del grid de 3 filas
    {
        auto p = panelBounds().reduced (22);
        p.removeFromTop (58 + 3 * 55 + 20);
        return p.removeFromTop (108);
    }
    juce::Rectangle<int> ioRow (int row) const   // row 0 = inicio, 1 = fin
    {
        auto a = inOutArea(); a.removeFromTop (30);       // debajo del título
        const int rh = 34, gap = 10;
        a.removeFromTop (row * (rh + gap));
        return a.removeFromTop (rh);
    }
    juce::Rectangle<int> ioTglRect (int row)   const { return ioRow (row).removeFromLeft (30).withSizeKeepingCentre (28, 28); }
    juce::Rectangle<int> ioLabelRect (int row) const { auto r = ioRow (row); r.removeFromLeft (38); return r.removeFromLeft (96); }
    juce::Rectangle<int> ioEditRect (int row)  const { auto r = ioRow (row); r.removeFromLeft (38 + 96); return r.removeFromLeft (90).withSizeKeepingCentre (90, 30); }
    juce::Rectangle<int> padArea() const   // Pad Player, debajo de inicio/fin
    {
        auto p = panelBounds().reduced (22);
        p.removeFromTop (58 + 3 * 55 + 20 + 108 + 14);
        return p.removeFromTop (96);
    }
    juce::Rectangle<int> padRow (int row) const
    {
        auto a = padArea(); a.removeFromTop (30);
        const int rh = 30, gap = 8;
        a.removeFromTop (row * (rh + gap));
        return a.removeFromTop (rh);
    }
    juce::Rectangle<int> padTglRect (int row)   const { return padRow (row).removeFromLeft (30).withSizeKeepingCentre (28, 28); }
    juce::Rectangle<int> padLabelRect (int row) const { auto r = padRow (row); r.removeFromLeft (38); return r; }
    juce::Rectangle<int> bibListArea() const
    {
        auto p = panelBounds().reduced (18); p.removeFromTop (52 + 44);   // título + buscador
        p.removeFromBottom (4);
        return p;
    }
    int bibMaxScroll() const { return juce::jmax (0, bib.size() * 46 - bibListArea().getHeight()); }
    juce::Rectangle<int> bibRect (int i) const
    {
        auto p = bibListArea();
        return { p.getX(), p.getY() + i * 46 - bibScroll, p.getWidth(), 40 };
    }
    juce::Rectangle<int> searchRect() const
    {
        auto p = panelBounds().reduced (18); p.removeFromTop (50);
        return p.removeFromTop (36);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xC0000000));
        auto p = panelBounds().toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (p, 14.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (p, 14.0f, 1.2f);

        g.setColour (juce::Colours::white); g.setFont (juce::Font (17.0f, juce::Font::bold));
        auto title = (mode == Biblioteca) ? juce::String::fromUTF8 ("Agregar canci\xc3\xb3n")
                                          : (juce::String::fromUTF8 ("Tono \xc2\xb7 ") + songTitle);
        auto tarea = panelBounds().removeFromTop (52).reduced (22, 0);
        if (mode == Tono) tarea = tarea.withTrimmedLeft (92).withTrimmedRight (48);   // lugar para Atrás (izq) y × (der)
        else              tarea = tarea.withTrimmedRight (48);                        // lugar para la ×
        g.drawText (title, tarea, juce::Justification::centredLeft);

        if (mode == Biblioteca)
        {
            const auto la = bibListArea();
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (la);
            for (int i = 0; i < bib.size(); ++i)
            {
                auto ri = bibRect (i);
                if (ri.getBottom() < la.getY() || ri.getY() > la.getBottom()) continue;
                auto r = ri.toFloat();
                g.setColour (juce::Colour (0xff1c1c1c)); g.fillRoundedRectangle (r, 9.0f);
                g.setColour (juce::Colour (0x22ffffff)); g.drawRoundedRectangle (r, 9.0f, 1.0f);
                // miniatura de portada a la izquierda
                auto thumb = r.reduced (5.0f); thumb = thumb.removeFromLeft (thumb.getHeight());
                if (bib[i].cover.isValid())
                {
                    juce::Graphics::ScopedSaveState ssi (g);
                    juce::Path cl; cl.addRoundedRectangle (thumb, 6.0f); g.reduceClipRegion (cl);
                    g.drawImage (bib[i].cover, thumb,
                                 juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
                }
                else { g.setColour (juce::Colour (0xff2a2a2a)); g.fillRoundedRectangle (thumb, 6.0f); }
                auto txt = r.withTrimmedLeft (thumb.getWidth() + 15.0f).withTrimmedRight (10.0f);
                g.setColour (juce::Colours::white); g.setFont (juce::Font (14.0f, juce::Font::bold));
                g.drawText (bib[i].titulo, txt.removeFromTop (r.getHeight() * 0.55f), juce::Justification::bottomLeft);
                g.setColour (juce::Colour (0xffa3a3a3)); g.setFont (juce::Font (11.5f));
                g.drawText (bib[i].artista, txt, juce::Justification::topLeft);   // solo artista (el tono se elige al entrar)
            }
        }
        else
        {
            for (int i = 0; i < keys.size() && i < 12; ++i)
            {
                auto r = keyRect (i).toFloat(); auto& k = keys.getReference (i);
                g.setColour (k.rendered ? juce::Colour (0xff20301f) : juce::Colour (0xff181818)); g.fillRoundedRectangle (r, 9.0f);
                g.setColour (k.rendered ? juce::Colour (0x553ED66E) : juce::Colour (0x22ffffff)); g.drawRoundedRectangle (r, 9.0f, 1.0f);
                g.setColour (k.rendered ? juce::Colours::white : juce::Colour (0xff6a6a6a));
                g.setFont (juce::Font (16.0f, juce::Font::bold));
                g.drawText (k.nombre, r.withTrimmedBottom (k.rendered ? 0.0f : 13.0f), juce::Justification::centred);
                if (! k.rendered)
                { g.setColour (juce::Colour (0xff7a7a7a)); g.setFont (juce::Font (9.5f, juce::Font::bold));
                  g.drawText (juce::String::fromUTF8 ("generar"), r.removeFromBottom (16.0f), juce::Justification::centred); }
                if (k.sem == renderingSem)
                {
                    g.setColour (juce::Colour (0xAA000000)); g.fillRoundedRectangle (r, 9.0f);
                    g.setColour (juce::Colour (0xff7Cc6ff)); g.setFont (juce::Font (11.0f, juce::Font::bold));
                    g.drawText (progTotal > 0 ? (juce::String (progHechos) + "/" + juce::String (progTotal)) : juce::String::fromUTF8 ("\xe2\x80\xa6"),
                                r, juce::Justification::centred);
                }
            }
            if (renderingSem != 99)   // barra de progreso
            {
                auto pb = panelBounds();
                juce::Rectangle<float> bar ((float) pb.getX() + 22.0f, (float) pb.getBottom() - 38.0f, (float) pb.getWidth() - 44.0f, 8.0f);
                g.setColour (juce::Colour (0xff262626)); g.fillRoundedRectangle (bar, 4.0f);
                float frac = progTotal > 0 ? juce::jlimit (0.05f, 1.0f, (float) progHechos / (float) progTotal) : 0.1f;
                g.setColour (juce::Colour (0xff3ED66E)); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * frac), 4.0f);
            }

            // #2 sección de inicio/fin (opcional)
            if (renderingSem == 99)
            {
                auto ttl = inOutArea().removeFromTop (24);
                g.setColour (juce::Colour (0xffcfcfcf)); g.setFont (juce::Font (13.0f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("Punto de inicio y final (opcional)"), ttl, juce::Justification::centredLeft);
                g.setColour (juce::Colour (0xffe8e8e8)); g.setFont (juce::Font (13.0f));
                g.drawText (juce::String::fromUTF8 ("Iniciar en"), ioLabelRect (0), juce::Justification::centredLeft);
                g.drawText (juce::String::fromUTF8 ("Terminar en"), ioLabelRect (1), juce::Justification::centredLeft);
                g.setColour (juce::Colour (0xff777777)); g.setFont (juce::Font (10.5f));
                g.drawText ("min:seg", ioEditRect (0).translated (0, -18).withHeight (16), juce::Justification::centred);

                // Pad Player (intro / outro)
                auto pttl = padArea().removeFromTop (24);
                g.setColour (juce::Colour (0xffcfcfcf)); g.setFont (juce::Font (13.0f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("Pad Player (autom\xc3\xa1tico)"), pttl, juce::Justification::centredLeft);
                g.setColour (juce::Colour (0xffe8e8e8)); g.setFont (juce::Font (13.0f));
                g.drawText (juce::String::fromUTF8 ("Pad al iniciar (se desvanece)"), padLabelRect (0), juce::Justification::centredLeft);
                g.drawText (juce::String::fromUTF8 ("Pad al finalizar"),              padLabelRect (1), juce::Justification::centredLeft);
            }
        }
    }

    void resized() override
    {
        closeBtn.setBounds (panelBounds().getRight() - 46, panelBounds().getY() + 12, 34, 30);   // × siempre arriba-der
        backBtn.setVisible (mode == Tono);
        backBtn.setBounds (panelBounds().getX() + 14, panelBounds().getY() + 12, 96, 30);         // ‹ Atrás solo en tono
        searchBox.setBounds (searchRect());

        inTgl.setBounds  (ioTglRect (0));  inEdit.setBounds  (ioEditRect (0));
        outTgl.setBounds (ioTglRect (1));  outEdit.setBounds (ioEditRect (1));
        padIntroTgl.setBounds (padTglRect (0));
        padOutroTgl.setBounds (padTglRect (1));
        refreshInOut();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelBounds().contains (e.getPosition())) { stopTimer(); renderingSem = 99; setVisible (false); return; }
        if (renderingSem != 99) return;   // ocupado renderizando
        if (mode == Biblioteca)
        {
            if (! bibListArea().contains (e.getPosition())) return;
            for (int i = 0; i < bib.size(); ++i)
                if (bibRect (i).contains (e.getPosition())) { if (onPickSong) onPickSong (bib[i].id); return; }
        }
        else
        {
            for (int i = 0; i < keys.size() && i < 12; ++i)
                if (keyRect (i).contains (e.getPosition()))
                {
                    auto& k = keys.getReference (i);
                    if (k.rendered) { if (onChoose) onChoose (k.sem, k.nombre); }
                    else            startRender (i);
                    return;
                }
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (mode != Biblioteca || ! bibListArea().contains (e.getPosition())) return;
        double d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        if (w.isReversed) d = -d;
        bibScroll = juce::jlimit (0, bibMaxScroll(), bibScroll + (int) (d * 300.0));
        repaint();
    }

    void startRender (int i)
    {
        renderingSem = keys[i].sem; pendIdx = i; progHechos = 0; progTotal = 0;
        const auto url = serverUrl, tok = token; const int sid = songId, sem = keys[i].sem;
        juce::Thread::launch ([url, tok, sid, sem]
        { httpPostForm (url + "/api/live/render/" + juce::String (sid) + "/" + juce::String (sem), {}, tok); });
        startTimerHz (2);
        repaint();
    }

    void timerCallback() override
    {
        if (renderingSem == 99) { stopTimer(); return; }
        const auto url = serverUrl, tok = token; const int sid = songId, sem = renderingSem;
        juce::Component::SafePointer<RepEditPanel> sp (this);
        juce::Thread::launch ([sp, url, tok, sid, sem]
        {
            auto est = juce::JSON::parse (httpGet (url + "/api/live/render/" + juce::String (sid) + "/" + juce::String (sem) + "/estado", tok));
            const bool listo = (bool) est.getProperty ("listo", false);
            const auto prog = est.getProperty ("progreso", "").toString();
            juce::MessageManager::callAsync ([sp, listo, prog]
            {
                if (sp == nullptr) return;
                if (prog.containsChar ('/')) { sp->progHechos = prog.upToFirstOccurrenceOf ("/", false, false).getIntValue();
                                               sp->progTotal  = prog.fromFirstOccurrenceOf ("/", false, false).getIntValue(); }
                if (listo)
                {
                    const int i = sp->pendIdx, sem2 = sp->renderingSem;
                    juce::String nom = (i >= 0 && i < sp->keys.size()) ? sp->keys[i].nombre : juce::String();
                    if (i >= 0 && i < sp->keys.size()) sp->keys.getReference (i).rendered = true;
                    sp->renderingSem = 99; sp->stopTimer(); sp->repaint();
                    if (sp->onChoose) sp->onChoose (sem2, nom);
                }
                else sp->repaint();
            });
        });
    }
};

// ───────── Página del músico (servida por el :5050 embebido) ─────────
static const char* kMusicianPage = R"HTMLPAGE(<!doctype html><html lang="es"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>MI Worship · En vivo</title>
<style>
 :root{--bg:#0a0a0a;--surface:#141414;--raised:#1f1f1f;--line:#2a2a2a;--txt:#ffffff;--txt2:#a3a3a3;--txt3:#666666;--accent:#9CA3AF;--accent-soft:rgba(156,163,175,0.16);--accent-ink:#000;--live:#86B36A;--lyric-size:18px;--chord-size:17px;--chord-min:21px;}
 body[data-size="xs"]{--lyric-size:14px;--chord-size:13px;--chord-min:17px;}
 body[data-size="s"]{--lyric-size:16px;--chord-size:15px;--chord-min:19px;}
 body[data-size="m"]{--lyric-size:18px;--chord-size:17px;--chord-min:21px;}
 body[data-size="l"]{--lyric-size:22px;--chord-size:20px;--chord-min:24px;}
 body[data-size="xl"]{--lyric-size:26px;--chord-size:24px;--chord-min:28px;}
 *{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
 html,body{height:100%;overflow:hidden}
 body{background:var(--bg);color:var(--txt);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;line-height:1.5;-webkit-text-size-adjust:100%}
 body.claro{--bg:#f4f4f6;--surface:#ffffff;--raised:#ececef;--line:#dcdce2;--txt:#15151a;--txt2:#5b5b66;--txt3:#8a8a95}
 body.solo-acordes .lyric{display:none}
 body.solo-letra .chord{display:none}
 body[data-grosor="fino"] .chord{font-weight:400;-webkit-text-stroke:0}
 body[data-grosor="normal"] .chord{font-weight:700;-webkit-text-stroke:0}
 body[data-grosor="grueso"] .chord{font-weight:800;-webkit-text-stroke:.45px currentColor}
 .wrap{height:100dvh;display:flex;flex-direction:column;padding:14px;padding-bottom:max(14px,env(safe-area-inset-bottom))}
 .topbar{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:10px;gap:10px}
 .song-title-row{display:flex;align-items:baseline;gap:8px;flex-wrap:wrap;min-width:0}
 .song-title{font-size:17px;font-weight:600;line-height:1.2}
 .song-meta{font-size:12px;color:var(--txt2);margin-top:2px}
 .tono-chip{display:inline-flex;align-items:center;justify-content:center;background:var(--accent);color:var(--accent-ink);font-weight:700;font-size:15px;padding:2px 10px;border-radius:6px;font-family:ui-monospace,Menlo,monospace;line-height:1.3;letter-spacing:.5px}
 .right{display:flex;align-items:center;gap:10px;flex-shrink:0}
 .live{width:9px;height:9px;border-radius:50%;background:#444;display:inline-block}
 .live.on{background:var(--live);box-shadow:0 0 8px var(--live)}
 .dots-btn{width:36px;height:32px;border-radius:7px;background:var(--surface);border:1px solid var(--line);color:var(--txt2);font-size:18px;line-height:1;cursor:pointer;font-family:inherit}
 .dots-btn:active{background:var(--raised);color:var(--txt)}
 .timeline-wrap{overflow-x:auto;-webkit-overflow-scrolling:touch;margin:0 -14px;padding:0 14px 6px;flex-shrink:0;scrollbar-width:none}
 .timeline-wrap::-webkit-scrollbar{display:none}
 .timeline{display:inline-flex;gap:6px;padding-bottom:4px;white-space:nowrap}
 .pill{display:inline-block;padding:6px 12px;border-radius:7px;font-size:13px;color:var(--txt2);background:var(--surface);border:1px solid var(--line);cursor:pointer;flex-shrink:0}
 .pill.done{color:var(--txt3)}
 .pill.active{color:var(--accent);background:var(--accent-soft);border-color:var(--accent);font-weight:600}
 body.claro .pill.active{background:var(--accent);border-color:var(--accent);color:var(--accent-ink)}
 .stage{flex:1;overflow-y:auto;padding:2px 0 40vh;-webkit-overflow-scrolling:touch;scroll-behavior:smooth}
 .sec{background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:14px 16px 16px;margin-bottom:12px;transition:border-color .2s}
 .sec.active{border-color:var(--accent);box-shadow:0 0 0 1px var(--accent)}
 .sechead{display:inline-flex;align-items:center;background:var(--raised);border-left:3px solid var(--line);border-radius:7px;padding:7px 13px;margin-bottom:10px;font-size:11px;font-weight:600;letter-spacing:1.8px;text-transform:uppercase;color:var(--txt3)}
 .sec.active .sechead{color:var(--accent);border-left-color:var(--accent)}
 .secnote{font-size:13px;color:var(--txt2);font-style:italic;margin-bottom:12px}
 .lyrics .line{display:flex;flex-wrap:wrap;align-items:flex-end;margin-bottom:6px}
 .lyrics .line.inst-line{gap:9px;align-items:center;margin:2px 0 12px}
 .lyrics .tok{display:inline-flex;flex-direction:column}
 .chord{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace;font-size:var(--chord-size);font-weight:700;color:var(--chord-color,var(--accent));min-height:var(--chord-min);line-height:var(--chord-min);white-space:pre}
 .lyric{font-size:var(--lyric-size);color:var(--txt);line-height:1.35;white-space:pre}
 .inst{display:flex;flex-wrap:wrap;gap:10px;align-items:center;padding:10px 0}
 .chip{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:var(--chord-size);font-weight:700;color:var(--accent);background:var(--accent-soft);padding:8px 16px;border-radius:8px}
 .inst-label{font-size:15px;color:var(--txt2);margin-bottom:14px}
 .off{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;color:var(--txt2);font-size:15px;padding:20px;text-align:center;background:var(--bg);z-index:50}
 .sheet-bg{position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:100;opacity:0;pointer-events:none;transition:opacity .2s}
 .sheet-bg.open{opacity:1;pointer-events:auto}
 .sheet{position:fixed;left:0;right:0;bottom:0;z-index:101;background:var(--surface);border-top:1px solid var(--line);border-radius:20px 20px 0 0;padding:18px 18px calc(22px + env(safe-area-inset-bottom));transform:translateY(110%);transition:transform .25s ease;max-height:82vh;overflow-y:auto}
 .sheet.open{transform:translateY(0)}
 .sheet-h{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
 .sheet-t{font-size:16px;font-weight:700}
 .sheet-x{background:none;border:none;color:var(--accent);font-size:15px;font-weight:600;cursor:pointer;font-family:inherit}
 .aj-lbl{font-size:11px;color:var(--txt2);text-transform:uppercase;letter-spacing:1.2px;font-weight:600;margin:16px 0 8px}
 .seg{display:flex;gap:6px;flex-wrap:wrap}
 .seg button{flex:1;min-width:72px;background:var(--raised);border:1px solid var(--line);color:var(--txt);padding:10px 8px;border-radius:8px;font-size:13px;cursor:pointer;font-family:inherit}
 .seg button.active{background:var(--accent);color:var(--accent-ink);border-color:var(--accent);font-weight:600}
 .seg.colores{gap:9px}
 .seg.colores button{flex:0 0 auto;min-width:0;width:34px;height:34px;border-radius:50%;padding:0;border:2px solid var(--line)}
 .seg.colores button.active{border-color:var(--txt)}
 .seg.colores input[type=color]{width:34px;height:34px;border:2px solid var(--line);border-radius:50%;padding:0;background:none;cursor:pointer}
 .seg.colores input[type=color]::-webkit-color-swatch-wrapper{padding:0}
 .seg.colores input[type=color]::-webkit-color-swatch{border:none;border-radius:50%}
 .size-row{display:flex;align-items:center;gap:16px;justify-content:center;margin-top:8px}
 .size-big{width:64px;height:52px;border-radius:12px;background:var(--raised);border:1px solid var(--line);color:var(--txt);font-size:20px;font-weight:700;cursor:pointer;font-family:inherit}
 .size-big:active{background:var(--accent-soft)}
 .size-big:disabled{opacity:.35}
 .size-lbl{font-size:16px;color:var(--accent);font-weight:700;min-width:40px;text-align:center;font-family:ui-monospace,Menlo,monospace}
</style></head><body>
<div class="wrap">
 <div class="topbar">
  <div style="min-width:0">
   <div class="song-title-row"><span class="song-title" id="t">—</span><span class="tono-chip" id="k" style="display:none"></span></div>
   <div class="song-meta" id="m"></div>
  </div>
  <div class="right">
   <span class="live" id="lv"></span>
   <button class="dots-btn" onclick="abrirAjustes()" title="Ajustes">&#8943;</button>
  </div>
 </div>
 <div class="timeline-wrap"><div class="timeline" id="tl"></div></div>
 <div class="stage" id="stage"></div>
</div>
<div class="off" id="off">Esperando al reproductor&#8230;</div>

<div class="sheet-bg" id="bg-ajustes" onclick="cerrarAjustes()"></div>
<div class="sheet" id="sheet-ajustes">
 <div class="sheet-h"><div class="sheet-t">Ajustes de vista</div><button class="sheet-x" onclick="cerrarAjustes()">Listo</button></div>
 <div class="aj-lbl">Tamaño de letra</div>
 <div class="size-row"><button class="size-big" id="size-minus" onclick="tamMenos()">A&#8722;</button><span class="size-lbl" id="size-label">M</span><button class="size-big" id="size-plus" onclick="tamMas()">A+</button></div>
 <div class="aj-lbl">Tema</div>
 <div class="seg" id="seg-tema"><button data-val="oscuro" onclick="setVista('tema','oscuro')">Oscuro</button><button data-val="claro" onclick="setVista('tema','claro')">Día</button></div>
 <div class="aj-lbl">Mostrar</div>
 <div class="seg" id="seg-modo"><button data-val="ambos" onclick="setVista('modo','ambos')">Ambos</button><button data-val="acordes" onclick="setVista('modo','acordes')">Solo acordes</button><button data-val="letra" onclick="setVista('modo','letra')">Solo letra</button></div>
 <div class="aj-lbl">Color de acordes</div>
 <div class="seg colores" id="seg-color">
  <button data-val="" onclick="setVista('color','')" title="Por defecto" style="background:var(--accent)"></button>
  <button data-val="#ffffff" onclick="setVista('color','#ffffff')" title="Blanco" style="background:#ffffff"></button>
  <button data-val="#ffd23f" onclick="setVista('color','#ffd23f')" title="Amarillo" style="background:#ffd23f"></button>
  <button data-val="#8fd3ff" onclick="setVista('color','#8fd3ff')" title="Celeste" style="background:#8fd3ff"></button>
  <button data-val="#8fe0a0" onclick="setVista('color','#8fe0a0')" title="Verde" style="background:#8fe0a0"></button>
  <button data-val="#ff9d5c" onclick="setVista('color','#ff9d5c')" title="Naranja" style="background:#ff9d5c"></button>
 </div>
 <div class="aj-lbl">Grosor de acordes</div>
 <div class="seg" id="seg-grosor"><button data-val="fino" onclick="setVista('grosor','fino')">Fino</button><button data-val="normal" onclick="setVista('grosor','normal')">Normal</button><button data-val="grueso" onclick="setVista('grosor','grueso')">Grueso</button></div>
 <div style="margin-top:16px;text-align:center"><button onclick="cambiarMusico()" style="background:none;border:none;color:var(--accent);font-size:13px;font-weight:600;cursor:pointer;font-family:inherit;text-decoration:underline">Cambiar de músico</button></div>
</div>

<div id="who-bg" style="position:fixed;inset:0;background:rgba(0,0,0,.78);z-index:50;display:none"></div>
<div id="who" style="position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);z-index:51;background:#141414;border:1px solid #2a2a2a;border-radius:16px;padding:22px;width:min(90vw,420px);max-height:80vh;overflow:auto;display:none">
 <div style="font-size:20px;font-weight:700;color:#fff;margin-bottom:4px">&#191;Qui&#233;n sos?</div>
 <div style="font-size:13px;color:#a3a3a3;margin-bottom:16px">Eleg&#237; tu nombre para ver los charts con tu configuraci&#243;n.</div>
 <div id="who-list" style="display:flex;flex-direction:column;gap:8px"></div>
</div>

<script>
var song=null, ver=-1, idx=-1;
var NIVELES=['xs','s','m','l','xl'], nivel=2;
function aplicarTam(){ document.body.dataset.size=NIVELES[nivel]; try{localStorage.setItem('np_tam',NIVELES[nivel]);}catch(e){} var lb=document.getElementById('size-label'); if(lb)lb.textContent=NIVELES[nivel].toUpperCase(); var bm=document.getElementById('size-minus'),bp=document.getElementById('size-plus'); if(bm)bm.disabled=(nivel===0); if(bp)bp.disabled=(nivel===NIVELES.length-1); }
function tamMenos(){ if(nivel>0){nivel--;aplicarTam();} }
function tamMas(){ if(nivel<NIVELES.length-1){nivel++;aplicarTam();} }
try{ var sv=localStorage.getItem('np_tam'); if(sv&&NIVELES.indexOf(sv)>=0) nivel=NIVELES.indexOf(sv); }catch(e){}
// ── Ajustes de vista (tema / mostrar / color / grosor) ──
var VISTA={tema:'oscuro',modo:'ambos',color:'',grosor:'normal'};
function marcarV(id,val){ var s=document.getElementById(id); if(!s)return; s.querySelectorAll('button').forEach(function(b){ b.classList.toggle('active', b.dataset.val===val); }); }
function aplicarVista(){
 document.body.classList.toggle('claro', VISTA.tema==='claro');
 document.body.classList.toggle('solo-acordes', VISTA.modo==='acordes');
 document.body.classList.toggle('solo-letra', VISTA.modo==='letra');
 if(VISTA.color){ document.body.style.setProperty('--chord-color', VISTA.color); } else { document.body.style.removeProperty('--chord-color'); }
 document.body.dataset.grosor=VISTA.grosor;
 marcarV('seg-tema',VISTA.tema); marcarV('seg-modo',VISTA.modo); marcarV('seg-color',VISTA.color); marcarV('seg-grosor',VISTA.grosor);
}
function cargarVista(){ try{ VISTA.tema=localStorage.getItem('charts_tema')||'oscuro'; VISTA.modo=localStorage.getItem('charts_modo')||'ambos'; VISTA.color=localStorage.getItem('charts_color')||''; VISTA.grosor=localStorage.getItem('charts_grosor')||'normal'; if(['fino','normal','grueso'].indexOf(VISTA.grosor)<0)VISTA.grosor='normal'; }catch(e){} aplicarVista(); }
function setVista(clave,val){ VISTA[clave]=val; try{localStorage.setItem('charts_'+clave,val);}catch(e){} aplicarVista(); }
function hexRgbaV(hex,a){ var h=hex.replace('#',''); if(h.length===3)h=h[0]+h[0]+h[1]+h[1]+h[2]+h[2]; return 'rgba('+parseInt(h.substr(0,2),16)+','+parseInt(h.substr(2,2),16)+','+parseInt(h.substr(4,2),16)+','+a+')'; }
function inkFor(hex){ var h=hex.replace('#',''); if(h.length===3)h=h[0]+h[0]+h[1]+h[1]+h[2]+h[2]; var r=parseInt(h.substr(0,2),16),g=parseInt(h.substr(2,2),16),b=parseInt(h.substr(4,2),16); return (0.299*r+0.587*g+0.114*b)>145?'#000':'#fff'; }
function aplicarAcento(){}
function setAcento(val){ try{ if(val)localStorage.setItem('mw_acento',val); else localStorage.removeItem('mw_acento'); }catch(e){} aplicarAcento(); }
function abrirAjustes(){ document.getElementById('bg-ajustes').classList.add('open'); document.getElementById('sheet-ajustes').classList.add('open'); }
function cerrarAjustes(){ document.getElementById('bg-ajustes').classList.remove('open'); document.getElementById('sheet-ajustes').classList.remove('open'); }
// ── Identidad del músico (Solución A: elegí tu nombre una vez; aplica tu config de la cuenta MiWorship) ──
var PERFILES=[];
function cargarPerfiles(cb){ try{ fetch('/perfiles',{cache:'no-store'}).then(function(r){return r.json();}).then(function(a){ PERFILES=Array.isArray(a)?a:[]; if(cb)cb(); }).catch(function(){ if(cb)cb(); }); }catch(e){ if(cb)cb(); } }
function aplicarPerfil(p){ try{ if(p.acento){localStorage.setItem('mw_acento',p.acento);}else{localStorage.removeItem('mw_acento');} var pr={}; try{pr=JSON.parse(p.prefs||'{}')||{};}catch(e){} if(pr.tema)localStorage.setItem('charts_tema',pr.tema); if(pr.modo)localStorage.setItem('charts_modo',pr.modo); if(pr.color!==undefined&&pr.color!==null)localStorage.setItem('charts_color',pr.color); if(pr.grosor)localStorage.setItem('charts_grosor',pr.grosor); if(pr.tam){localStorage.setItem('np_tam',pr.tam); var ni=NIVELES.indexOf(pr.tam); if(ni>=0)nivel=ni;} }catch(e){} cargarVista(); aplicarAcento(); aplicarTam(); }
function elegirMusico(id){ try{localStorage.setItem('np_musico_id',String(id));}catch(e){} var p=PERFILES.filter(function(x){return String(x.id)===String(id);})[0]; if(p)aplicarPerfil(p); cerrarWho(); }
function mostrarWho(){ var l=document.getElementById('who-list'); l.innerHTML=PERFILES.length?PERFILES.map(function(p){return '<button onclick="elegirMusico('+p.id+')" style="text-align:left;padding:14px 16px;border-radius:10px;border:1px solid #2a2a2a;background:#1f1f1f;color:#fff;font-size:16px;font-weight:600;cursor:pointer;font-family:inherit">'+esc(p.nombre)+'</button>';}).join(''):'<div style="color:#888;padding:12px;text-align:center">No hay perfiles disponibles.<br>Conect&#225; NeuralPlay a internet una vez para bajar la lista.</div>'; document.getElementById('who-bg').style.display='block'; document.getElementById('who').style.display='block'; }
function cerrarWho(){ document.getElementById('who-bg').style.display='none'; document.getElementById('who').style.display='none'; }
function cambiarMusico(){ cerrarAjustes(); cargarPerfiles(mostrarWho); }
function initIdentidad(){ cargarPerfiles(function(){ var id=null; try{id=localStorage.getItem('np_musico_id');}catch(e){} var p=id?PERFILES.filter(function(x){return String(x.id)===String(id);})[0]:null; if(p){ aplicarPerfil(p); } else { mostrarWho(); } }); }
// ── Chart ──
function esc(s){return String(s==null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
function renderLines(lines){return '<div class="lyrics">'+(lines||[]).map(function(line){line=line||[];var solo=line.length>0&&line.every(function(t){return !((t[1]||'').trim());});if(solo){return '<div class="line inst-line">'+line.filter(function(t){return (t[0]||'').trim();}).map(function(t){return '<span class="chip">'+esc(t[0])+'</span>';}).join('')+'</div>';}return '<div class="line">'+line.map(function(t){return '<div class="tok"><span class="chord">'+esc(t[0])+'</span><span class="lyric">'+esc((t[1]==null||t[1]==='')?' ':t[1])+'</span></div>';}).join('')+'</div>';}).join('')+'</div>';}
function renderBody(s){ if(s.inst&&s.prog){return '<div class="inst-label">Instrumental</div><div class="inst">'+s.prog.map(function(a){return '<span class="chip">'+esc(a)+'</span>';}).join('')+'</div>';} return renderLines(s.lines); }
function renderSong(){
 var secs=song.secciones||[];
 document.getElementById('t').textContent=song.titulo||'—';
 document.getElementById('m').textContent=(song.tempo?song.tempo+' BPM':'')+(song.compas?'   ·   '+song.compas:'');
 var k=document.getElementById('k'); if(song.tono){k.textContent=song.tono;k.style.display='';}else{k.style.display='none';}
 document.getElementById('stage').innerHTML=secs.map(function(s,i){return '<div class="sec" id="sec-'+i+'"><div class="sechead">'+esc(s.tipo)+'</div>'+(s.nota?'<div class="secnote">'+esc(s.nota)+'</div>':'')+renderBody(s)+'</div>';}).join('');
 document.getElementById('tl').innerHTML=secs.map(function(s,i){return '<span class="pill" id="pill-'+i+'" onclick="irSec('+i+')">'+esc(s.tipo)+'</span>';}).join('');
 idx=-1;
}
function irSec(i){ var t=document.getElementById('sec-'+i), st=document.getElementById('stage'); if(t&&st){ st.scrollTop=t.offsetTop-(st.firstElementChild?st.firstElementChild.offsetTop:0); } }
function setActive(i){
 if(!song||!song.secciones||i<0||i>=song.secciones.length||i===idx) return;
 idx=i;
 var secs=song.secciones;
 for(var s=0;s<secs.length;s++){ var el=document.getElementById('sec-'+s); if(el)el.classList.toggle('active',s===i); var pl=document.getElementById('pill-'+s); if(pl){pl.classList.toggle('active',s===i);pl.classList.toggle('done',s<i);} }
 var st=document.getElementById('stage'), target=document.getElementById('sec-'+i);
 if(st&&target){ st.scrollTop=target.offsetTop-(st.firstElementChild?st.firstElementChild.offsetTop:0); }
}
async function loadSong(){ try{ var r=await fetch('/song',{cache:'no-store'}); var j=await r.json(); if(j&&j.ok!==false&&j.secciones){ song=j; renderSong(); } }catch(e){} }
async function tick(){
 try{
  var r=await fetch('/state',{cache:'no-store'}); var s=await r.json();
  document.getElementById('off').style.display='none';
  if(s.ver!==ver){ ver=s.ver; await loadSong(); }
  document.getElementById('lv').classList.toggle('on',!!s.playing);
  if(song){ setActive(s.idx); }
 }catch(e){
  var o=document.getElementById('off'); o.style.display='flex'; o.textContent='Esperando al reproductor…';
 }
}
aplicarTam(); cargarVista(); aplicarAcento();
initIdentidad();
setInterval(tick,400); tick();
</script></body></html>)HTMLPAGE";

// ───────── Servidor HTTP embebido (músicos se conectan a http://<ip-mac>:5050) ─────────
struct HttpLiveServer : private juce::Thread
{
    HttpLiveServer() : juce::Thread ("LiveHTTP") {}
    ~HttpLiveServer() override { stop(); }

    int port = 5050;
    std::function<juce::String()> getPage, getSong, getState, getPerfiles;
    juce::StreamingSocket listener;

    void start() { if (! isThreadRunning()) startThread(); }
    void stop()
    {
        signalThreadShouldExit();
        listener.close();
        stopThread (1500);
    }

    void run() override
    {
        if (! listener.createListener (port))
            return;
        while (! threadShouldExit())
        {
            std::unique_ptr<juce::StreamingSocket> c (listener.waitForNextConnection());
            if (c == nullptr) { if (threadShouldExit()) break; continue; }
            handle (*c);
        }
    }

    void handle (juce::StreamingSocket& s)
    {
        if (s.waitUntilReady (true, 500) <= 0) { s.close(); return; }
        char buf[2048];
        int n = s.read (buf, sizeof (buf) - 1, false);
        if (n <= 0) { s.close(); return; }
        buf[n] = 0;
        juce::String req = juce::String::fromUTF8 (buf, n);
        juce::String path = "/";
        auto sp = req.indexOfChar (' ');
        if (sp >= 0)
        {
            auto rest = req.substring (sp + 1);
            auto sp2 = rest.indexOfChar (' ');
            path = (sp2 >= 0 ? rest.substring (0, sp2) : rest);
        }
        auto q = path.indexOfChar ('?');
        if (q >= 0) path = path.substring (0, q);

        juce::String body, ctype = "text/html; charset=utf-8";
        bool nostore = false;
        if (path == "/" || path == "/index.html")            body = getPage  ? getPage()  : juce::String();
        else if (path == "/song")  { ctype = "application/json; charset=utf-8"; nostore = true; body = getSong  ? getSong()  : juce::String ("{}"); }
        else if (path == "/state") { ctype = "application/json; charset=utf-8"; nostore = true; body = getState ? getState() : juce::String ("{}"); }
        else if (path == "/perfiles") { ctype = "application/json; charset=utf-8"; nostore = true; body = getPerfiles ? getPerfiles() : juce::String ("[]"); }
        else { writeResp (s, "404 Not Found", "text/plain; charset=utf-8", "no encontrado", false); return; }
        writeResp (s, "200 OK", ctype, body, nostore);
    }

    void writeResp (juce::StreamingSocket& s, const char* status, const juce::String& ctype,
                    const juce::String& body, bool nostore)
    {
        const int len = body.getNumBytesAsUTF8();
        juce::String hdr;
        hdr << "HTTP/1.1 " << status << "\r\n"
            << "Content-Type: " << ctype << "\r\n"
            << "Content-Length: " << len << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n";
        if (nostore) hdr << "Cache-Control: no-store\r\n";
        hdr << "Connection: close\r\n\r\n";
        auto h = hdr.toRawUTF8();
        s.write (h, (int) std::strlen (h));
        if (len > 0) s.write (body.toRawUTF8(), len);
        s.close();
    }
};

// ── Panel de la vista de faders del Pad (selector + Auto/12 tonos + fader + descarga) ──
struct PadPanel : public juce::Component
{
    juce::String packName;
    juce::Image  portada;
    int   sel = 0;            // 0 = Auto, 1..12 = tono (sel-1)
    int   playingIdx = -1;    // tono que suena ahora (resalta en Auto)
    bool  enabled = false;
    int   readyMask = 0;      // bits 0..11 de tonos descargados
    bool  havePack = false;
    bool  armMode = false;    // modo MIDI: el click sobre el fader lo arma
    int   faderCc = 0;        // CC asignado (>0 = asignado)
    bool  faderArmed = false; // armando el fader ahora

    std::function<void()>    onOpenMenu, onFaderArm;
    std::function<void(int)> onSel;    // -1 = Auto/Link ; 0..11 = tono

    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };

    PadPanel()
    {
        fader.setRange (-60.0, 0.0, 0.1);
        fader.setValue (0.0, juce::dontSendNotification);
        addAndMakeVisible (fader);
    }
    void setArmMode (bool on) { armMode = on; fader.setInterceptsMouseClicks (! on, ! on); repaint(); }

    juce::Rectangle<int> rLink, rPort, rName, rStatus, rFader, rFaderLbl;
    juce::Rectangle<int> rCell[12];

    static const char* noteName (int i)
    {
        static const char* N[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return (i >= 0 && i < 12) ? N[i] : "";
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        auto fcol = r.removeFromRight (74);
        rFaderLbl = fcol.removeFromBottom (20);
        rFader = fcol.reduced (10, 8);
        fader.setBounds (rFader);
        r.removeFromRight (12);

        auto top = r.removeFromTop (60);
        rLink = top.removeFromLeft (58).reduced (0, 8);
        top.removeFromLeft (10);
        rName = top.reduced (0, 6);                    // la barra de vidrio ocupa el resto
        { auto inner = rName.reduced (6); rPort = inner.removeFromLeft (inner.getHeight()); }   // portada cuadrada DENTRO de la barra
        r.removeFromTop (8);
        rStatus = r.removeFromTop (18);
        r.removeFromTop (10);

        const int cols = 3, rows = 4, gap = 10;
        const int cw  = (r.getWidth()  - (cols - 1) * gap) / cols;
        const int chh = (r.getHeight() - (rows - 1) * gap) / rows;
        for (int i = 0; i < 12; ++i)
        {
            const int cx = i % cols, cy = i / cols;
            rCell[i] = juce::Rectangle<int> (r.getX() + cx * (cw + gap),
                                             r.getY() + cy * (chh + gap), cw, chh);
        }
    }

    void drawCell (juce::Graphics& g, juce::Rectangle<int> b, const juce::String& txt,
                   bool active, bool playing, bool ready)
    {
        auto rf = b.toFloat();
        juce::Colour bg = active ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff232323);
        if (! ready) bg = bg.withAlpha (0.30f);
        g.setColour (bg);
        g.fillRoundedRectangle (rf, 12.0f);
        if (playing && ! active) { g.setColour (juce::Colour (0xff2E8BFF)); g.drawRoundedRectangle (rf.reduced (1.5f), 12.0f, 2.5f); }
        g.setColour (active ? juce::Colours::black : juce::Colour (0xfff2f2f2));
        g.setFont (juce::Font (24.0f, juce::Font::bold));
        g.drawText (txt, ready ? b : b.withTrimmedBottom (14), juce::Justification::centred);
        if (! ready)
        {
            g.setColour (juce::Colour (0xffb0b0b0));
            g.setFont (juce::Font (10.0f));
            g.drawText ("bajando...", b.removeFromBottom (14), juce::Justification::centred);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0f0f0f));
        // Link (Auto: sigue la canción)
        {
            const bool on = (sel == 0);
            g.setColour (on ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff232323));
            g.fillRoundedRectangle (rLink.toFloat(), 9.0f);
            g.setColour (on ? juce::Colours::black : juce::Colour (0xfff2f2f2));
            g.setFont (juce::Font (13.0f, juce::Font::bold));
            g.drawText ("Link", rLink, juce::Justification::centred);
        }
        // barra de vidrio templado con la portada DENTRO + nombre (tap -> lista de pads)
        {
            auto rf = rName.toFloat();
            g.setColour (juce::Colour (0x22ffffff)); g.fillRoundedRectangle (rf, 10.0f);
            g.setColour (juce::Colour (0x18ffffff)); g.fillRoundedRectangle (rf.withHeight (rf.getHeight() * 0.5f), 10.0f);
            g.setColour (juce::Colour (0x40ffffff)); g.drawRoundedRectangle (rf.reduced (0.5f), 10.0f, 1.0f);
            // portada con esquinas redondeadas, dentro de la cajita
            auto pf = rPort.toFloat();
            g.setColour (juce::Colour (0xff181818)); g.fillRoundedRectangle (pf, 7.0f);
            if (portada.isValid())
            {
                juce::Graphics::ScopedSaveState ss (g);
                juce::Path clip; clip.addRoundedRectangle (pf, 7.0f);
                g.reduceClipRegion (clip);
                g.drawImage (portada, pf, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
            }
            g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (pf.reduced (0.5f), 7.0f, 1.0f);
            // nombre + chevron
            juce::Rectangle<int> txt (rPort.getRight() + 12, rName.getY(),
                                      juce::jmax (10, rName.getRight() - 24 - (rPort.getRight() + 12)), rName.getHeight());
            g.setColour (juce::Colour (0xfff2f2f2));
            g.setFont (juce::Font (16.0f, juce::Font::bold));
            g.drawText (havePack ? packName : juce::String ("Elegir pad"), txt, juce::Justification::centredLeft);
            g.setFont (juce::Font (13.0f));
            g.drawText (juce::String::fromUTF8 ("\xe2\x96\xbe"), rName.reduced (10, 0).removeFromRight (18), juce::Justification::centredRight);
        }
        // estado de descarga
        int done = 0; for (int i = 0; i < 12; ++i) if (readyMask & (1 << i)) ++done;
        g.setColour (done >= 12 ? juce::Colour (0xff8fe0a0) : juce::Colour (0xffb0b0b0));
        g.setFont (juce::Font (12.0f));
        g.drawText (done >= 12 ? juce::String ("12 tonos listos") : ("Descargando " + juce::String (done) + "/12..."),
                    rStatus, juce::Justification::centredLeft);
        // 12 tonos
        for (int i = 0; i < 12; ++i)
            drawCell (g, rCell[i], noteName (i), sel == i + 1, (sel == 0 && playingIdx == i), (readyMask & (1 << i)) != 0);
        // fader
        g.setColour (juce::Colour (0xffb0b0b0));
        g.setFont (juce::Font (11.0f));
        g.drawText ("PAD", rFaderLbl, juce::Justification::centred);
        if (armMode)
        {
            auto bb = juce::Rectangle<int> (rFader.getX() - 1, rFader.getY() - 20, rFader.getWidth() + 2, 18);
            g.setColour (faderArmed ? juce::Colour (0xffB84BE6) : juce::Colour (0x99B84BE6));
            g.fillRoundedRectangle (bb.toFloat(), 5.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (9.5f, juce::Font::bold));
            g.drawText (faderArmed ? juce::String ("...") : (faderCc > 0 ? juce::String ("MIDI") : juce::String ("asignar")), bb, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto p = e.getPosition();
        if (armMode && rFader.contains (p)) { if (onFaderArm) onFaderArm(); return; }
        if (rLink.contains (p)) { if (onSel)      onSel (-1); return; }
        if (rName.contains (p)) { if (onOpenMenu) onOpenMenu(); return; }
        for (int i = 0; i < 12; ++i)
            if (rCell[i].contains (p)) { if ((readyMask & (1 << i)) && onSel) onSel (i); return; }
    }
};

class MainComponent : public juce::AudioAppComponent,
                      private juce::Timer,
                      private juce::ChangeListener,
                      private juce::MidiInputCallback
{
public:
    MainComponent()
    {
        loadConfig();
        setLookAndFeel (&pillLnf);
        setWantsKeyboardFocus (true);   // #4 recibir teclas para el mapping de teclado
        logoImg = juce::ImageFileFormat::loadFrom (BinaryData::AppIcon_png, (size_t) BinaryData::AppIcon_pngSize);
        splash.logo = logoImg;
        formatManager.registerBasicFormats();
       #if JUCE_MAC
        formatManager.registerFormat (new juce::CoreAudioFormat(), false);
       #endif
        readThread.startThread();
        thumb.addChangeListener (this);

        connectButton.setButtonText ("Conectar");
        connectButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        connectButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        connectButton.onClick = [this] { startLoad(); };
        addAndMakeVisible (connectButton);
        connectButton.setVisible (false);   // reemplazado por los iconos de cabecera

        settingsBtn.kind = 3; repertoireBtn.kind = 4;
        addAndMakeVisible (settingsBtn);
        addAndMakeVisible (repertoireBtn);
        repertoireBtn.onClick = [this] { openRepertoirePicker(); };
        repPicker.onLoad = [this] (juce::String id) { startLoadId (id); };
        repPicker.onNew  = [this] { createSetlist(); };
        repPicker.onDelete = [this] (juce::String id) { confirmDeleteSetlist (id); };
        repPicker.onSave = [this] (juce::String id) { saveRepertoireMixes (id); };
        repPicker.onDownload = [this] (juce::String id) { downloadRepertoireOffline (id); };
        addChildComponent (repPicker);

        settingsBtn.onClick = [this]
        {
            settingsPanel.setState (syncEnabled, syncLinked.load());
            settingsPanel.setBounds (getLocalBounds());
            settingsPanel.setVisible (true);
            settingsPanel.toFront (true);
        };
        settingsPanel.onSync    = [this] (bool on) { setSync (on); };
        settingsPanel.onConfig  = [this] { settingsPanel.setVisible (false); openAudioConfig(); };
        settingsPanel.onRefresh = [this] { settingsPanel.setVisible (false); reloadCurrent(); };
        settingsPanel.onStorage = [this] { settingsPanel.setVisible (false); openStorage(); };
        settingsPanel.onCountIn = [this] (bool on) { countInEnabled = on; saveStorageCfg(); };
        settingsPanel.onMasterPS = [this] (bool on)
        {
            masterPerSong = on; saveStorageCfg();
            if (! on && currentSong >= 0) globalMasterDb = masterSlider.getValue();   // arranca el general desde el actual
        };
        settingsPanel.onMixPS = [this] (bool on)
        {
            mixPerSong = on; saveStorageCfg();
            if (! on) snapshotGlobalFromCurrent();     // arranca la mezcla general desde la actual
            applyGlobalOverrides();
        };
        addChildComponent (settingsPanel);

        loadStorageCfg();
        loadInOut();
        loadPadPlayer();
        loadKeyMap();
        loadMidiMap();
        openMidiInputs();
        loadClickSec();
        settingsPanel.setCountIn (countInEnabled);
        settingsPanel.setMasterPS (masterPerSong);
        settingsPanel.setMixPS (mixPerSong);
        storagePanel.onFreeUnused = [this] { deleteUnusedCache(); };
        storagePanel.onAutoClean  = [this] (bool on) { cacheAutoClean = on; saveStorageCfg(); storagePanel.setStats (storagePanel.total, storagePanel.unused, cacheAutoClean, cacheCapGB); if (on) { deleteUnusedCache(); enforceCap(); } };
        storagePanel.onCap        = [this] (int gb) { cacheCapGB = gb; saveStorageCfg(); storagePanel.setStats (storagePanel.total, storagePanel.unused, cacheAutoClean, cacheCapGB); enforceCap(); refreshStorageStats(); };
        addChildComponent (storagePanel);

        audioCfg.onDevice = [this] (const juce::String& d) { applyAudioDevice (d); };
        audioCfg.onRoute  = [this] (int f, int m, int b)   { setFamRoute (f, m, b); };
        audioCfg.onSampleRate = [this] (double sr)
        {
            preferredSampleRate = sr;
            applyAudioDevice (audioOutDevice.isEmpty() ? currentDeviceName() : audioOutDevice);
        };
        audioCfg.onBack = [this]
        {
            settingsPanel.setState (syncEnabled, syncLinked.load());
            settingsPanel.setBounds (getLocalBounds());
            settingsPanel.setVisible (true);
            settingsPanel.toFront (true);
        };
        addChildComponent (audioCfg);

        liveServer.getPage  = [] { return juce::String (juce::CharPointer_UTF8 (kMusicianPage)); };
        liveServer.getSong  = [this] { const juce::ScopedLock l (chartLock); return currentChartJson; };
        liveServer.getPerfiles = [this] { const juce::ScopedLock l (chartLock); return perfilesJson; };
        liveServer.getState = [this]
        {
            juce::String j;
            j << "{\"idx\":" << liveSectionIdx.load()
              << ",\"ver\":" << liveSongVer.load()
              << ",\"playing\":" << (playing.load() ? "true" : "false") << "}";
            return j;
        };
        editBtn.setButtonText ("Editar");
        editBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        editBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        editBtn.onClick = [this] { toggleEdit(); };
        addAndMakeVisible (editBtn);

        keyMapBtn.setButtonText (juce::String::fromUTF8 ("Mapping de teclado"));   // #4 (barra de Editar)
        keyMapBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        keyMapBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        keyMapBtn.onClick = [this] { toggleKeyMapMode(); refreshMapButtons(); };
        addChildComponent (keyMapBtn);   // se muestra solo en la barra de Editar

        midiMapBtn.setButtonText (juce::String::fromUTF8 ("MIDI Mapping"));   // #5/#6
        midiMapBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        midiMapBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        midiMapBtn.onClick = [this] { toggleMidiMapMode(); refreshMapButtons(); };
        addChildComponent (midiMapBtn);

        faderStrip.onMouseDown = [this] (const juce::MouseEvent& e)   // en modo MIDI, el click arma el fader
        {
            if (! midiMapMode) return;
            const auto& sl = (faderView == 1) ? busSliders : trackSliders;
            for (int i = 0; i < sl.size(); ++i)
                if (sl[i]->isVisible() && sl[i]->getBounds().contains (e.getPosition()))
                {
                    if (faderView == 1) armFaderIfMidi (-1, (i < familyNames.size() ? familyNames[i] : juce::String()), false);
                    else                armFaderIfMidi (i, {}, false);
                    return;
                }
        };


        addCard.onClick = [this] { openBibliotecaForAdd(); };
        addChildComponent (addCard);

        repEdit.onPickSong = [this] (int songId)
        {
            juce::String title;
            for (auto& b : bibliotecaAll) if (b.id == songId) { title = b.titulo; break; }
            openTonoFor (songId, title, true);
        };
        repEdit.onChoose = [this] (int sem, juce::String nombre)
        {
            juce::ignoreUnused (sem);
            const int sid = repEdit.songId; const bool add = repEdit.addFlow;
            repEdit.setVisible (false);
            if (add) addSong (sid, nombre);
            else     setSongTono (sid, nombre);
        };
        repEdit.onNeedCovers = [this] (juce::Array<RepEditPanel::BibItem> items) { loadBibCovers (items); };
        repEdit.onInOut = [this] (int sid, double inS, double outS)   // #2 guardar inicio/fin
        {
            if (inS < 0.0 && outS < 0.0) songInOut.erase (sid);
            else                         songInOut[sid] = { inS, outS };
            saveInOut();
            if (currentSong >= 0 && currentSong < repertoire.size()
                && repertoire.getReference (currentSong).id == sid)
            {
                aplicarInOut (sid);                                  // aplicar en vivo si es la canción actual
                if (! playing.load() && inS > 0.0) seekSeconds (inS);
            }
        };
        repEdit.onPadPlayer = [this] (int sid, bool intro, bool outro)   // Pad Player por canción
        {
            if (! intro && ! outro) songPad.erase (sid);
            else                    songPad[sid] = { intro, outro };
            savePadPlayer();
            if (currentSong >= 0 && currentSong < repertoire.size()
                && repertoire.getReference (currentSong).id == sid)
            {
                curPadIntro.store (intro);
                curPadOutro.store (outro);
            }
        };
        addChildComponent (repEdit);
        padBtn.setButtonText ("PAD");
        padBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        padBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        padBtn.onClick = [this] { if (clickOrArm (kaPad)) return; padManualToggle(); };
        addAndMakeVisible (padBtn);

        connStatus.setJustificationType (juce::Justification::centred);
        connStatus.setColour (juce::Label::textColourId, juce::Colour (0xffa3a3a3));
        connStatus.setFont (juce::Font (12.0f));
        addAndMakeVisible (connStatus);

        playButton.setButtonText ("Play");
        playButton.onClick = [this] { if (clickOrArm (kaPlay)) return; togglePlay(); };
        addAndMakeVisible (playButton);

        returnButton.setButtonText ("|" + juce::String::charToString ((juce_wchar) 0x25C0));
        returnButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        returnButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        returnButton.onClick = [this] { if (clickOrArm (kaReturn)) return; seekSeconds (0.0); };
        addAndMakeVisible (returnButton);

        for (auto* b : { &barPrevBtn, &barNextBtn })   // navegación por compás sobre el mapa
        {
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xcc1a1a1a));
            b->setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
            addAndMakeVisible (b);
        }
        barPrevBtn.setButtonText (juce::String::charToString ((juce_wchar) 0x25C0));   // ◀
        barNextBtn.setButtonText (juce::String::charToString ((juce_wchar) 0x25B6));   // ▶
        barPrevBtn.onClick = [this] { if (clickOrArm (kaPrevBar)) return; seekSection (-1); };
        barNextBtn.onClick = [this] { if (clickOrArm (kaNextBar)) return; seekSection (+1); };

        fadeButton.onClick = [this] { if (clickOrArm (kaFade)) return; toggleFade(); };
        addAndMakeVisible (fadeButton);

        timeLabel.setJustificationType (juce::Justification::centred);
        timeLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
        timeLabel.setFont (juce::Font (12.0f, juce::Font::bold));
        timeLabel.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (timeLabel);

        masterSlider.setSliderStyle (juce::Slider::LinearVertical);
        masterSlider.setRange (-60.0, 0.0, 0.1);   // tope = volumen original (sin boost)
        masterSlider.setValue (0.0);
        masterSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        masterSlider.textFromValueFunction = [] (double v) { return dbText (v); };
        masterSlider.valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -60.0 : t.getDoubleValue(); };
        masterSlider.onValueChange = [this]
        {
            const double v = masterSlider.getValue();
            masterGain.store (dbToGain ((float) v));
            markDirty();
            if (masterPerSong)
            {
                if (currentSong >= 0 && currentSong < songMaster.size()) songMaster.set (currentSong, v);
            }
            else { globalMasterDb = v; saveStorageCfg(); }   // master general del setlist
        };
        masterGain.store (dbToGain (0.0f));
        masterSlider.setLookAndFeel (&faderLnf);
        masterSlider.setRepaintsOnMouseActivity (false);
        addAndMakeVisible (masterSlider);
        masterLabel.setText ("Master", juce::dontSendNotification);
        masterLabel.setJustificationType (juce::Justification::centred);
        masterLabel.setColour (juce::Label::textColourId, juce::Colour (0xffffffff));
        addAndMakeVisible (masterLabel);

        busesBtn.setButtonText ("Buses");
        padPlayerBtn.setButtonText ("Pad");
        muteMidiBtn.setButtonText ("MIDI");
        for (auto* tb : { &busesBtn, &padPlayerBtn, &muteMidiBtn })
        {
            tb->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
            tb->setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
            addAndMakeVisible (tb);
        }
        padPlayerBtn.onClick = [this] { if (clickOrArm (kaPadPlayer)) return; setFaderView (3); };
        faderViewBtn.kind = 0; repeatBtn.kind = 1; infiniteBtn.kind = 2;
        faderViewBtn.active = true;
        addAndMakeVisible (faderViewBtn);
        addAndMakeVisible (repeatBtn);
        addAndMakeVisible (infiniteBtn);
        faderViewBtn.onClick = [this] { if (clickOrArm (kaFaderView)) return; setFaderView (0); };
        busesBtn.onClick     = [this] { if (clickOrArm (kaBuses))     return; setFaderView (1); };
        muteMidiBtn.onClick  = [this] { if (clickOrArm (kaMidi))      return; setFaderView (2); };

        repeatBtn.onClick   = [this] { if (clickOrArm (kaRepeat)) return; toggleRepeatOnce(); };
        infiniteBtn.onClick = [this] { if (clickOrArm (kaLoop))   return; toggleLoopInfinite(); };

        for (auto& b : busGain) b.store (1.0f);

        faderStrip.onPaint = [this] (juce::Graphics& g) { paintFaderStrip (g); };
        faderViewport.setViewedComponent (&faderStrip, false);
        faderViewport.setScrollBarsShown (false, true);
        faderViewport.setScrollBarThickness (10);
        addAndMakeVisible (faderViewport);
        addAndMakeVisible (midiPanel);
        midiPanel.setVisible (false);
        midiPanel.loadCfg (npAppDir().getChildFile ("midi.json"));
        midiPanel.onChanged = [this] { rebuildMidiOuts(); repaint (mapBounds); };

        addAndMakeVisible (padPanel);
        padPanel.setVisible (false);
        padPanel.fader.setLookAndFeel (&faderLnf);
        padPanel.onOpenMenu = [this] { showPadPackMenu(); };
        padPanel.onFaderArm = [this] { armPadFader(); };
        padPanel.onSel    = [this] (int t)
        {
            if (padAutoActive || padOutroLatched) padUserOverride = true;   // el usuario toma el control
            padAutoActive = false; padOutroLatched = false; padAutoFadeOutStarted = false;
            if (t < 0) padMode = 0;                        // Auto (sigue la canción)
            else       { padMode = 1; padManualIdx = t; }  // tono fijo
            saveStorageCfg();
            if (! padEnabled.load()) setPadEnabled (true);
            else                     padApplyTone();
            updatePadUi();
        };
        padPanel.fader.onValueChange = [this]
        {
            padGainDb = padPanel.fader.getValue();
            padGain.store (dbToGain ((float) padGainDb));
            saveStorageCfg();
        };
        rebuildMidiOuts();
        midiClock.tick = [this] { fireMidiRT(); };
        midiClock.startTimer (1);   // ~1 ms: minima latencia MIDI, hilo dedicado

        setSize (1040, 906);
        setAudioChannels (0, 32);   // hasta 32 salidas (interfaces multicanal)
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.bufferSize = 256;   // menor latencia de salida (sin comprometer estabilidad)
        setup.sampleRate = 0.0;   // 0 = automático: seguir la frecuencia nativa del dispositivo
        deviceManager.setAudioDeviceSetup (setup, true);

        // Enrutamiento de salida: cargar config y aplicar dispositivo guardado (si está conectado)
        loadAudioRouting();
        {
            const juce::String want = audioOutDevice;
            const auto avail = outputDeviceNames();
            if (want.isNotEmpty() && avail.contains (want))
                applyAudioDevice (want);
            else
            {
                audioOutDevice = currentDeviceName();
                ensureDeviceRoutes (audioOutDevice);
                openOutChans = juce::jlimit (1, 32, currentOutputChannelCount());
                snapshotRoutes();
            }
        }

        splashStart = juce::Time::getMillisecondCounter();
        addAndMakeVisible (splash);
        startTimerHz (60);

        if (serverToken.isNotEmpty()) startLoad();
        else connStatus.setText ("Falta config (servidor/token)", juce::dontSendNotification);
    }

    ~MainComponent() override
    {
        liveServer.stop();
        if (syncEnabled && serverUrl.isNotEmpty() && serverToken.isNotEmpty())
        {
            juce::String body = "{\"token\":\"" + serverToken + "\",\"ip\":\""
                                + localLanIp() + "\",\"accion\":\"bye\"}";
            httpPostJson (serverUrl + "/api/live_ping", body);   // avisar apagado del puente
        }
        midiClock.stopTimer();
        flushMidiOffs();
        if (loader)     loader->stopThread (6000);
        if (mixBuilder) mixBuilder->stopThread (2000);
        for (auto* s : trackSliders) s->setLookAndFeel (nullptr);
        masterSlider.setLookAndFeel (nullptr);
        padPanel.fader.setLookAndFeel (nullptr);
        setLookAndFeel (nullptr);
        thumb.removeChangeListener (this);
        shutdownAudio();
    }

    struct PadVoice {
        std::unique_ptr<juce::AudioFormatReaderSource> src;
        std::unique_ptr<juce::BufferingAudioSource>    buf;
        std::unique_ptr<juce::ResamplingAudioSource>   res;
        double fileRate = 44100.0;
        float  gain = 0.0f, target = 0.0f;   // envolvente de crossfade
        int    idx = -1;
        bool   dead = false;                 // marcada en el hilo de audio; se libera en el timer (fuera del audio)
    };
    struct PadPack { juce::String id, nombre, portada; int baseIdx = 0; bool listo = false; };

    void prepareToPlay (int spb, double sampleRate) override
    {
        const juce::ScopedLock sl (graphLock);
        deviceSampleRate = sampleRate;
        currentBlockSize = spb;
        temp.setSize (2, juce::jmax (spb, 2048) + 8);
        for (int i = 0; i < resamplers.size(); ++i)
        {
            resamplers[i]->setResamplingRatio (fileRates[i] / sampleRate);
            resamplers[i]->prepareToPlay (spb, sampleRate);
        }
        {
            const juce::ScopedLock pl (padLock);
            padTemp.setSize (2, juce::jmax (spb, 2048) + 8);
            for (auto* pv : padVoices) if (pv->res) { pv->res->setResamplingRatio (pv->fileRate / sampleRate); pv->res->prepareToPlay (spb, sampleRate); }
        }
        prepared.store (true);
    }
    void releaseResources() override
    {
        prepared.store (false);
        for (auto* r : resamplers) r->releaseResources();
        { const juce::ScopedLock pl (padLock); for (auto* pv : padVoices) if (pv->res) pv->res->releaseResources(); }
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        renderTracks (info);   // mezcla de la canción (puede salir temprano y dejar el buffer en silencio)
        mixPad (info);         // el pad SIEMPRE se mezcla encima (suena aunque no haya canción o esté en pausa)
    }

    void mixPad (const juce::AudioSourceChannelInfo& info)
    {
        const juce::ScopedTryLock pl (padLock);
        if (! pl.isLocked() || padVoices.isEmpty()) return;
        const int nn = info.numSamples;
        if (padTemp.getNumSamples() < nn) return;   // no asignar en el hilo de audio (ya dimensionado en prepareToPlay)
        auto* b = info.buffer;
        if (b->getNumChannels() < 1) return;
        float* oL = b->getWritePointer (0, info.startSample);
        float* oR = b->getNumChannels() > 1 ? b->getWritePointer (1, info.startSample) : nullptr;
        const float mg = masterGain.load();
        const float pg0 = padGainCur;                 // ganancia del fader al inicio del bloque
        const float pg1 = padGain.load();             // objetivo (se interpola => sin zipper)
        const float step = (float) (nn / juce::jmax (1.0, deviceSampleRate * padXfadeSec));
        float dbgPeak = 0.0f;
        for (int v = padVoices.size(); --v >= 0;)
        {
            auto* pv = padVoices.getUnchecked (v);
            if (pv->dead || pv->res == nullptr) { pv->dead = true; continue; }   // muerta: no mezclar ni borrar aquí
            padTemp.clear();
            juce::AudioSourceChannelInfo pi (&padTemp, 0, nn);
            pv->res->getNextAudioBlock (pi);
            const float* sL = padTemp.getReadPointer (0);
            const float* sR = padTemp.getNumChannels() > 1 ? padTemp.getReadPointer (1) : sL;
            const float g0 = pv->gain;
            float g1 = pv->gain;
            if      (pv->gain < pv->target) g1 = juce::jmin (pv->target, pv->gain + step);
            else if (pv->gain > pv->target) g1 = juce::jmax (pv->target, pv->gain - step);
            pv->gain = g1;
            for (int n = 0; n < nn; ++n)
            {
                const float f  = (nn > 1) ? (float) n / (float) (nn - 1) : 1.0f;
                const float gg = (g0 + (g1 - g0) * f) * (pg0 + (pg1 - pg0) * f) * mg;
                const float s  = sL[n] * gg;
                oL[n] += s;
                if (oR) oR[n] += sR[n] * gg;
                const float aa = std::abs (s); if (aa > dbgPeak) dbgPeak = aa;
            }
            if (pv->target <= 0.0f && pv->gain <= 0.0002f) pv->dead = true;   // se libera en el timer (fuera del audio)
        }
        padGainCur = pg1;
        padDbgVoices.store (padVoices.size());
        padDbgMg.store (mg);
        padDbgAbs.store (dbgPeak);
        for (int n = 0; n < nn; ++n) { oL[n] = softClip (oL[n]); if (oR) oR[n] = softClip (oR[n]); }
    }

    // Limpia (fuera del hilo de audio) las voces marcadas 'dead' en mixPad. Se llama desde timerCallback (hilo de mensajes).
    void reapDeadPadVoices()
    {
        const juce::ScopedLock pl (padLock);
        for (int v = padVoices.size(); --v >= 0;)
            if (padVoices.getUnchecked (v)->dead)
                padVoices.remove (v);   // aquí SÍ se libera memoria: message thread, seguro
    }

    // ── Control del pad (hilo de mensajes / fondo) ───────────────────
    static int rootIndexOf (const juce::String& key)
    {
        auto s = key.trim();
        if (s.isEmpty()) return -1;
        int base;
        switch (s[0]) { case 'C': base = 0; break; case 'D': base = 2; break; case 'E': base = 4; break;
                        case 'F': base = 5; break; case 'G': base = 7; break; case 'A': base = 9; break;
                        case 'B': base = 11; break; default: return -1; }
        if (s.length() > 1) { if (s[1] == '#') base += 1; else if (s[1] == 'b') base -= 1; }
        return ((base % 12) + 12) % 12;
    }
    int padSongRootIdx() const
    {
        if (currentSong < 0 || currentSong >= repertoire.size()) return -1;
        return rootIndexOf (repertoire.getReference (currentSong).tonoNombre);
    }
    juce::File padCacheFile (const juce::String& pack, int idx) const
    {
        return npAppDir().getChildFile ("pads").getChildFile (pack)
                         .getChildFile ("tono_" + juce::String (idx) + ".wav");
    }
    void buildPadVoice (const juce::File& f, int idx, int gen, bool instant = false)
    {
        auto* reader = formatManager.createReaderFor (f);
        if (reader == nullptr) return;
        auto voice = std::make_unique<PadVoice>();
        voice->fileRate = reader->sampleRate > 0 ? reader->sampleRate : 44100.0;
        voice->idx = idx;
        voice->gain = instant ? 1.0f : 0.0f;              // instant = a full (sin fade-in)
        auto* rs = new juce::AudioFormatReaderSource (reader, true);
        rs->setLooping (true);
        voice->src.reset (rs);
        voice->buf.reset (new juce::BufferingAudioSource (rs, readThread, false, 88200, 2));
        voice->res.reset (new juce::ResamplingAudioSource (voice->buf.get(), false, 2));
        const double sr = deviceSampleRate; const int bs = juce::jmax (256, currentBlockSize);
        voice->res->setResamplingRatio (voice->fileRate / sr);
        voice->res->prepareToPlay (bs, sr);
        voice->target = 1.0f;
        const juce::ScopedLock pl (padLock);
        if (padJobGen.load() != gen) return;              // hubo otro cambio: descartar
        for (auto* pv : padVoices) pv->target = 0.0f;     // fade-out de los anteriores
        padVoices.add (voice.release());
    }
    void requestPadTone (int idx, bool instant = false)    // 0-11 · instant = arranca a full (sin swell)
    {
        if (padPackId.isEmpty() || idx < 0 || idx > 11) return;
        padPlayingIdx.store (idx);
        const int gen = padJobGen.fetch_add (1) + 1;
        const juce::String url = serverUrl + "/api/live/pad/" + padPackId + "/" + juce::String (idx);
        const juce::String tok = serverToken;
        const juce::File dest = padCacheFile (padPackId, idx);
        juce::Thread::launch ([this, gen, url, tok, dest, idx, instant]
        {
            if (! dest.existsAsFile())
            {
                dest.getParentDirectory().createDirectory();
                httpDownload (url, tok, dest);
            }
            if (padJobGen.load() != gen || ! dest.existsAsFile()) return;
            buildPadVoice (dest, idx, gen, instant);
        });
    }
    // Aplica el tono según el modo (Auto = raíz de la canción, Manual = elegido)
    void padApplyTone()
    {
        int idx = (padMode == 0) ? padSongRootIdx() : padManualIdx;
        if (idx < 0) idx = padPackBaseIdx;                 // sin canción: usa el tono base
        requestPadTone (idx);
    }
    void setPadEnabled (bool on)
    {
        padEnabled.store (on);
        if (on) { padApplyTone(); }
        else    { padJobGen.fetch_add (1); padPlayingIdx.store (-1);
                  const juce::ScopedLock pl (padLock); for (auto* pv : padVoices) pv->target = 0.0f; }
    }
    void padManualToggle()   // toggle manual: el usuario toma el control y libera la automatización de la zona
    {
        if (padAutoActive || padOutroLatched) padUserOverride = true;
        padAutoActive = false;
        padOutroLatched = false;
        padAutoFadeOutStarted = false;
        setPadEnabled (! padEnabled.load());
        updatePadUi();
    }

    // ── Pad Player: automatización intro/outro por canción ───────────
    // Todo se hace con la propia voz del pad (fade natural ~3s): sin capa extra => sin glitches.
    void endPadAuto()
    {
        if (! padAutoActive && ! padOutroLatched) return;
        padAutoActive = false;
        padAutoTurnedOn = false;
        padAutoFadeOutStarted = false;
        padOutroLatched = false;
        if (padEnabled.load()) { setPadEnabled (false); updatePadUi(); }   // se apaga con el fade de la voz
    }
    void updatePadAutomation()
    {
        const bool intro = curPadIntro.load();
        const bool outro = curPadOutro.load();
        const bool wasPlaying = padPrevPlaying;
        padPrevPlaying = playing.load();
        if ((! intro && ! outro) || currentSong < 0) { endPadAuto(); padUserOverride = false; return; }

        const double p      = positionSeconds();
        const double startS = juce::jmax (0.0, songInSec.load());
        const double total  = totalSeconds();
        const double endRef = (songOutSec.load() > 0.0) ? songOutSec.load() : total;   // final efectivo
        const double barSec = (bpm > 0.0) ? (juce::jmax (1, beatsPerBar) * 60.0 / bpm) : 0.0;
        const double outroStart = (barSec > 0.0) ? (endRef - 3.0 * barSec) : (endRef - 8.0);

        const bool nearStart     = (p >= startS - 0.05 && p < startS + padIntroInSec + padIntroOutSec);
        const bool inOutroWin    = outro && playing.load() && endRef > 0.0 && outroStart > startS && p >= outroStart - 0.02;
        const bool playStartEdge = playing.load() && ! wasPlaying;   // se le acaba de dar Play

        if (! nearStart && ! inOutroWin) padUserOverride = false;    // re-armar al salir de las zonas

        // INTRO: se dispara SOLO al ARRANCAR la reproducción cerca del inicio
        // (no en el loop automático, que debe sostener el outro).
        if (playStartEdge && nearStart && ! padUserOverride)
        {
            if (intro)
            {
                padOutroLatched = false;                 // volver a darle Play corta el outro sostenido
                padAutoActive = true;
                padAutoTurnedOn = ! padEnabled.load();
                padAutoFadeOutStarted = false;
                if (padAutoTurnedOn) { padMode = 0; setPadEnabled (true); updatePadUi(); }
            }
            else if (padOutroLatched) { endPadAuto(); }  // sin intro: el replay corta el outro
        }

        // INTRO en curso: entra (~3s) y baja (~3s), con el fade natural de la voz.
        if (intro && padAutoActive && ! padOutroLatched && nearStart && playing.load())
        {
            const double t = p - startS;
            const double fadeOutAt = padAutoTurnedOn ? padIntroInSec : 0.0;   // bed: baja de una
            if (t >= fadeOutAt && ! padAutoFadeOutStarted && padEnabled.load())
            {
                padAutoFadeOutStarted = true;
                setPadEnabled (false);
                updatePadUi();
            }
            return;
        }

        // OUTRO enganchado: se mantiene aunque la canción vuelva al inicio automáticamente.
        if (padOutroLatched)
        {
            if (padUserOverride) { padOutroLatched = false; padAutoActive = false; }
            else return;
        }

        // OUTRO: se activa en los ÚLTIMOS 3 COMPASES (solo reproduciendo) y se engancha.
        if (inOutroWin && ! padUserOverride)
        {
            if (! padAutoActive)
            {
                padAutoActive = true;
                padAutoTurnedOn = ! padEnabled.load();
                if (padAutoTurnedOn) { padMode = 0; setPadEnabled (true); updatePadUi(); }
            }
            padOutroLatched = true;
            return;
        }
        endPadAuto();
    }

    void renderTracks (const juce::AudioSourceChannelInfo& info)
    {
        info.clearActiveBufferRegion();
        const juce::ScopedTryLock sl (graphLock);
        if (! sl.isLocked() || loadingSong.load() || ! prepared.load() || resamplers.isEmpty())
            return;

        const long long sk = seekTo.exchange (-1);
        if (sk >= 0)
        {
            for (auto* b : bufferingSources) b->setNextReadPosition (sk);
            for (auto* r : resamplers)       r->flushBuffers();
            const double fr = fileRates.isEmpty() ? 44100.0 : fileRates[0];
            positionOut.store ((long long) (sk * deviceSampleRate / fr));
        }
        if (! playing.load()) return;

        const int nn = info.numSamples;
        const int nch = juce::jmax (1, info.buffer->getNumChannels());
        const int useCh = juce::jmin (nch, 32);
        float* out[32] = { nullptr };
        for (int c = 0; c < useCh; ++c) out[c] = info.buffer->getWritePointer (c, info.startSample);

        bool anySolo = false;
        for (int t = 0; t < resamplers.size(); ++t) if (trackSolo[t].load()) { anySolo = true; break; }

        float ciG = 1.0f;   // #1 conteo: swell de entrada (0->1) durante el compás previo a la sección
        if (countInActive.load())
        {
            const double p = (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate);
            const double a = countInStartSec.load(), b = countInEndSec.load();
            if (p >= b || b <= a) { ciG = 1.0f; countInActive.store (false); }
            else                  ciG = (float) juce::jlimit (0.0, 1.0, (p - a) / (b - a));
        }

        for (int t = 0; t < resamplers.size(); ++t)
        {
            temp.clear();
            juce::AudioSourceChannelInfo ti (&temp, 0, nn);
            resamplers.getUnchecked (t)->getNextAudioBlock (ti);
            const bool audible = ! trackMuted[t].load() && (! anySolo || trackSolo[t].load());
            const float cg = (t < kMaxTracks && trackNoFade[t]) ? 1.0f : ciG;   // Click/Guía no suben
            const float g = audible ? trackGain[t].load() * busGain[trackFamily[t]].load() * cg : 0.0f;
            const float* tL = temp.getReadPointer (0);
            const float* tR = temp.getNumChannels() > 1 ? temp.getReadPointer (1) : tL;

            const int fi   = (t < kMaxTracks ? trackRouteFam[t] : 10);   // familia -> ruta de salida
            const int mode = famMode[fi];
            const int base = famBaseCh[fi];
            float peak = 0.0f;

            if (mode == 2 && base >= 0 && base < useCh)            // estéreo (canal base + base+1)
            {
                float* aL = out[base];
                float* aR = (base + 1 < useCh) ? out[base + 1] : nullptr;
                for (int n = 0; n < nn; ++n)
                {
                    const float cl = tL[n] * g, cr = (tR != tL ? tR[n] : tL[n]) * g;
                    aL[n] += cl; if (aR) aR[n] += cr;
                    const float a = juce::jmax (std::abs (cl), std::abs (cr));
                    if (a > peak) peak = a;
                }
            }
            else if (mode == 1 && base >= 0 && base < useCh)       // mono (suma L+R a un canal)
            {
                float* aM = out[base];
                for (int n = 0; n < nn; ++n)
                {
                    const float cm = (tL[n] + (tR != tL ? tR[n] : tL[n])) * 0.5f * g;
                    aM[n] += cm;
                    const float a = std::abs (cm);
                    if (a > peak) peak = a;
                }
            }
            else                                                  // off: solo medimos nivel del track
            {
                for (int n = 0; n < nn; ++n) { const float a = std::abs (tL[n] * g); if (a > peak) peak = a; }
            }

            const float prev = trackLevel[t].load();
            trackLevel[t].store (peak > prev ? peak : prev * 0.82f);
        }

        // #3 metrónomo sintetizado en el bloque de click (después del final, donde no hay audio grabado)
        if (clickSecArmed.load() && bpm > 0.0)
        {
            const long long blockStart = positionOut.load();
            const double beatLen = deviceSampleRate * 60.0 / bpm;
            const double startAbs = totalSeconds() * deviceSampleRate;      // el bloque empieza en el final
            const int bpb = juce::jmax (1, beatsPerBar);
            int cb = -1, cm = 2;                                   // ruta de salida del click
            for (int t = 0; t < resamplers.size(); ++t)
                if (t < kMaxTracks && trackIsClick[t]) { const int fi = trackRouteFam[t]; cm = famMode[fi]; cb = famBaseCh[fi]; break; }
            if (cb < 0) { cb = 0; cm = 2; }
            float* cL = (cb >= 0 && cb < useCh) ? out[cb] : nullptr;
            float* cR = (cm == 2 && cb + 1 < useCh) ? out[cb + 1] : nullptr;
            const double twoPi = juce::MathConstants<double>::twoPi;
            for (int n = 0; n < nn; ++n)
            {
                const double rel = (double) (blockStart + n) - startAbs;
                if (rel >= 0.0)
                {
                    const long long bi = (long long) std::floor (rel / beatLen);
                    if (bi != clkLastBeat) { clkLastBeat = bi; clkEnv = 1.0; clkPhase = 0.0; clkFreq = ((bi % bpb) == 0) ? 1600.0 : 1050.0; }
                }
                if (clkEnv > 0.0002)
                {
                    const float s = (float) (std::sin (clkPhase) * clkEnv * 0.55);
                    if (cL) cL[n] += s;
                    if (cR) cR[n] += s;
                    clkPhase += twoPi * clkFreq / deviceSampleRate;
                    clkEnv *= 0.9990;
                }
            }
        }

        const float m = masterGain.load();
        for (int c = 0; c < useCh; ++c) { float* o = out[c]; for (int n = 0; n < nn; ++n) o[n] = softClip (o[n] * m); }
        positionOut.fetch_add (nn);

        const double posSec = (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate);
        const double fr2 = fileRates.isEmpty() ? 44100.0 : fileRates[0];
        if ((loopActive.load() || loopOnce.load()) && loopEndSec.load() > 0.0 && posSec >= loopEndSec.load())
        {
            seekTo.store ((long long) (juce::jmax (0.0, loopStartSec.load()) * fr2));
            if (loopOnce.load())
            {
                loopOnce.store (false);
                if (! loopActive.load()) { loopStartSec.store (-1.0); loopEndSec.store (-1.0); }
            }
        }
        else if (clickSecArmed.load() && clickLenSec.load() > 0.0)
        {   // #3 canción con sección de click: al pasar el final entra al bloque (metrónomo)
            const double bEnd = totalSeconds() + clickLenSec.load();
            if (posSec >= bEnd - 1.0e-4)
            {
                if (clickLoopOn.load())                                   // ∞ ON: loop del bloque
                {
                    seekTo.store ((long long) (totalSeconds() * fr2));    // volver al inicio del bloque (= final)
                    clkLastBeat = -1;
                }
                else
                {                                                        // ∞ OFF: al terminar, volver al inicio de la canción
                    playing.store (false);
                    const double inS = juce::jmax (0.0, songInSec.load());
                    seekTo.store ((long long) (inS * fr2));
                }
            }
        }
        else
        {
            const double outS = songOutSec.load();                       // #2 punto de salida (si hay)
            const double effEnd = (outS > 0.0) ? juce::jmin (outS, totalSeconds()) : totalSeconds();
            const long long endDev = (long long) (effEnd * deviceSampleRate);
            if (endDev > 0 && positionOut.load() >= endDev)
            {
                playing.store (false);
                const double inS = juce::jmax (0.0, songInSec.load());   // volver al inicio (o al punto de entrada)
                seekTo.store ((long long) (inS * fr2));
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0a0a));
        drawLogo (g, 28.0f, 18.5f);   // centrado vertical con los botones de la cabecera

        {
            auto tbx = timeLabel.getBounds().toFloat();
            if (! tbx.isEmpty())
            {
                g.setColour (juce::Colour (0xff1f1f1f));
                g.fillRoundedRectangle (tbx, 7.0f);
                g.setColour (juce::Colour (0x22ffffff));
                g.drawRoundedRectangle (tbx, 7.0f, 1.0f);
            }
        }

        // Caja de Tempo (arriba) / Compás (abajo), a la par de la caja de tiempo
        if (currentSong >= 0 && bpm > 0.0 && ! compasBoxBounds.isEmpty())
        {
            auto boxf = compasBoxBounds.toFloat();
            g.setColour (juce::Colour (0xff1f1f1f)); g.fillRoundedRectangle (boxf, 7.0f);
            g.setColour (juce::Colour (0x22ffffff)); g.drawRoundedRectangle (boxf, 7.0f, 1.0f);
            const int hh = compasBoxBounds.getHeight() / 2;
            g.setColour (juce::Colour (0xffe8e8e8));
            g.setFont (juce::Font (12.5f, juce::Font::bold));
            g.drawText (juce::String (juce::roundToInt (currentBpm())) + " BPM",
                        compasBoxBounds.withHeight (hh).translated (0, 1), juce::Justification::centred, false);
            g.setColour (juce::Colour (0xffb0b0b0));
            g.setFont (juce::Font (11.5f));
            g.drawText (songCompas, compasBoxBounds.withTrimmedTop (hh).translated (0, -1),
                        juce::Justification::centred, false);
        }

        // Nombre del repertorio centrado (debajo del botón de Play)
        if (! setlistBandBounds.isEmpty() && currentSetlistName.isNotEmpty())
        {
            g.setColour (juce::Colour (0xffcfcfcf));
            g.setFont (juce::Font (13.5f, juce::Font::bold));
            g.drawText (currentSetlistName, setlistBandBounds, juce::Justification::centred, true);
        }

        auto mb = mapBounds;
        g.setColour (juce::Colour (0xff0d0d0d));
        g.fillRoundedRectangle (mb.toFloat(), 8.0f);

        auto inner = mb.reduced (8);
        double vs = 0.0, ve = 0.0; getViewWindow (vs, ve);
        const double span = juce::jmax (0.001, ve - vs);

        if (bpm > 0.0)
        {
            const double secPerEighth = 30.0 / bpm;
            const int perBar = juce::jmax (1, beatsPerBar) * 2;
            for (int j = (int) std::floor (vs / secPerEighth); j * secPerEighth <= ve; ++j)
            {
                const double t = j * secPerEighth;
                if (t < vs || t < 0.0) continue;
                const float gx = inner.getX() + (float) ((t - vs) / span) * inner.getWidth();
                float a = 0.035f;
                if (j % 2 == 0) a = 0.085f;
                if (j % perBar == 0) a = 0.17f;
                g.setColour (juce::Colours::white.withAlpha (a));
                g.drawVerticalLine ((int) gx, (float) inner.getY(), (float) inner.getBottom());
            }
        }

        if (thumb.getTotalLength() > 0.0)
        {
            g.setColour (juce::Colour (0xffced1d6));
            thumb.drawChannels (g, inner, vs, ve, 1.2f);
        }
        else
        {
            g.setColour (juce::Colour (0xff8a94a6));
            g.setFont (13.0f);
            juce::String msg;
            if (currentSong >= 0)                     msg = juce::String ("Cargando forma de onda...");
            else if (! repertoire.isEmpty())          msg = juce::String::fromUTF8 ("Descargando repertorio\xe2\x80\xa6");
            else if (serverToken.isEmpty())           msg = juce::String ("Conecta para traer el repertorio");
            else if (currentSetlistName.isNotEmpty()) msg = juce::String::fromUTF8 ("Empez\xc3\xa1 a agregar canciones a este repertorio con +");
            else                                      msg = juce::String::fromUTF8 ("Abr\xc3\xad un repertorio o cre\xc3\xa1 uno nuevo");
            g.drawText (msg, inner, juce::Justification::centred, true);
        }

        {
            const double total = totalSeconds();
            const double posNow = positionSeconds();
            auto drawBlk = [&] (double t0, double t1, const juce::String& name)
            {
                if (t1 <= vs || t0 >= ve || t1 <= t0) return;
                const double c0 = juce::jmax (t0, vs);
                const double c1 = juce::jmin (t1, ve);
                const float x0 = inner.getX() + (float) ((c0 - vs) / span) * inner.getWidth();
                const float x1 = inner.getX() + (float) ((c1 - vs) / span) * inner.getWidth();
                juce::Rectangle<float> blk (x0 + 1.5f, (float) inner.getY() + 1.0f,
                                            juce::jmax (10.0f, x1 - x0 - 3.0f), (float) inner.getHeight() - 2.0f);
                const bool act = (posNow >= t0 && posNow < t1);
                g.setColour (act ? juce::Colour (0x22ffffff) : juce::Colour (0x07ffffff));
                g.fillRoundedRectangle (blk, 8.0f);
                g.setColour (act ? juce::Colour (0x99ffffff) : juce::Colour (0x33ffffff));
                g.drawRoundedRectangle (blk, 8.0f, 1.3f);
                const float tw = juce::jmin (blk.getWidth() - 8.0f, (float) name.length() * 7.2f + 14.0f);
                juce::Rectangle<float> chip (blk.getX() + 4.0f, blk.getY() + 3.0f, juce::jmax (18.0f, tw), 19.0f);
                g.setColour (juce::Colour (0xcc0c0c0c));
                g.fillRoundedRectangle (chip, 5.0f);
                g.setColour (act ? juce::Colours::white : juce::Colour (0xffe6e6e6));
                g.setFont (juce::Font (12.5f, juce::Font::bold));
                g.drawText (name, chip.reduced (6.0f, 0.0f), juce::Justification::centredLeft, true);

                const bool looped = (loopActive.load() || loopOnce.load())
                                    && std::abs (loopStartSec.load() - t0) < 0.06
                                    && std::abs (loopEndSec.load() - t1) < 0.06;
                if (looped)
                {
                    juce::Rectangle<float> cor (blk.getRight() - 25.0f, blk.getBottom() - 23.0f, 21.0f, 20.0f);
                    g.setColour (juce::Colour (0xff2E8BFF));
                    g.setFont (juce::Font (17.0f, juce::Font::bold));
                    g.drawText (juce::String::fromUTF8 (loopActive.load() ? "∞" : "↻"), cor, juce::Justification::centred);
                }
            };

            // Bloque inicial "Conteo" (antes de la primera seccion)
            if (! sectionTimes.isEmpty() && sectionTimes[0] > 0.4)
                drawBlk (0.0, sectionTimes[0], "Conteo");

            for (int i = 0; i < sectionTimes.size(); ++i)
            {
                const double t0 = sectionTimes[i];
                const double t1 = (i + 1 < sectionTimes.size() ? sectionTimes[i + 1] : total);
                drawBlk (t0, t1, sectionNames[i]);
            }

            // #3 sección de click: bloque anexo "Click ∞" + agregar/quitar (modo edición)
            addClickBtnRect = {}; delClickBtnRect = {};
            const double lastEnd = total;
            if (songHasClickSec)
            {
                const double cl = clickSecLen();
                if (cl > 0.0)
                {
                    drawBlk (lastEnd, lastEnd + cl, juce::String::fromUTF8 ("Click \xe2\x88\x9e"));
                    // ticks del click: un pulso por beat (8 en 2 compases 4/4), downbeats más marcados
                    const int nbeats = juce::jmax (2, beatsPerBar * 2);
                    for (int k = 0; k <= nbeats; ++k)
                    {
                        const double tt = lastEnd + cl * (double) k / (double) nbeats;
                        if (tt < vs || tt > ve) continue;
                        const float x = inner.getX() + (float) ((tt - vs) / span) * inner.getWidth();
                        const bool down = (k % juce::jmax (1, beatsPerBar)) == 0;
                        g.setColour (down ? juce::Colour (0xcc3ED66E) : juce::Colour (0x66ffffff));
                        g.fillRect (x - (down ? 1.2f : 0.7f), (float) inner.getY() + 26.0f,
                                    down ? 2.4f : 1.4f, (float) inner.getHeight() - 30.0f);
                    }
                    if (clickLoopOn.load() && positionSeconds() >= totalSeconds() - 0.05)   // ícono ∞ solo al estar en el bloque
                    {
                        const double c1 = juce::jmin (lastEnd + cl, ve);
                        if (c1 > vs)
                        {
                            const float x1 = inner.getX() + (float) ((c1 - vs) / span) * inner.getWidth();
                            juce::Rectangle<float> cor (x1 - 26.0f, (float) inner.getBottom() - 24.0f, 21.0f, 20.0f);
                            g.setColour (juce::Colour (0xff2E8BFF)); g.setFont (juce::Font (17.0f, juce::Font::bold));
                            g.drawText (juce::String::fromUTF8 ("\xe2\x88\x9e"), cor, juce::Justification::centred);
                        }
                    }
                }
                if (editMode && lastEnd <= ve + 0.5)   // − para eliminar
                {
                    const float xr = inner.getX() + (float) ((juce::jmin (lastEnd + cl, ve) - vs) / span) * inner.getWidth();
                    juce::Rectangle<int> pill (juce::jlimit (inner.getX(), inner.getRight() - 30, (int) xr - 30),
                                               inner.getY() + inner.getHeight() / 2 - 14, 28, 28);
                    delClickBtnRect = pill;
                    auto pf = pill.toFloat();
                    g.setColour (juce::Colour (0xffE5534B)); g.fillRoundedRectangle (pf, 7.0f);
                    g.setColour (juce::Colours::white); g.setFont (juce::Font (20.0f, juce::Font::bold));
                    g.drawText (juce::String::fromUTF8 ("\xe2\x88\x92"), pill, juce::Justification::centred);
                }
            }
            else if (editMode && currentSong >= 0 && ! resamplers.isEmpty() && lastEnd <= ve + 0.5)
            {   // + justo al final de la última sección
                const float xr = inner.getX() + (float) ((juce::jmin (lastEnd, ve) - vs) / span) * inner.getWidth();
                juce::Rectangle<int> pill (juce::jlimit (inner.getX(), inner.getRight() - 32, (int) xr + 4),
                                           inner.getY() + inner.getHeight() / 2 - 15, 30, 30);
                addClickBtnRect = pill;
                auto pf = pill.toFloat();
                g.setColour (juce::Colour (0xff2E6BE6)); g.fillRoundedRectangle (pf, 7.0f);
                g.setColour (juce::Colour (0x66000000)); g.drawRoundedRectangle (pf, 7.0f, 1.0f);
                g.setColour (juce::Colours::white); g.setFont (juce::Font (20.0f, juce::Font::bold));
                g.drawText ("+", pill, juce::Justification::centred);
            }
        }

        const double pos = positionSeconds();
        if (pos >= vs && pos <= ve)
        {
            const float px = inner.getX() + (float) ((pos - vs) / span) * inner.getWidth();
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRect (juce::Rectangle<float> ((float) inner.getX(), (float) inner.getY(), px - inner.getX(), (float) inner.getHeight()));
            g.setColour (juce::Colours::white);
            g.fillRect (px - 1.0f, (float) inner.getY(), 2.0f, (float) inner.getHeight());
        }

        // Marcadores de notas MIDI (cajas activas), en su tiempo, del color de la caja
        {
            const float by = (float) inner.getBottom() - 2.0f;
            for (int ci = 0; ci < currentMidiBoxes.size() && ci < 8; ++ci)
            {
                if (! midiPanel.isOn (ci)) continue;
                const auto col = cajaColour (ci);
                for (const auto& n : currentMidiBoxes[ci].notas)
                {
                    if (n.seg < vs || n.seg > ve) continue;
                    const float x = inner.getX() + (float) ((n.seg - vs) / span) * inner.getWidth();
                    g.setColour (col);
                    juce::Path tri; tri.addTriangle (x - 4.0f, by, x + 4.0f, by, x, by - 8.0f);
                    g.fillPath (tri);
                }
            }
        }

        // Panel de faders (vidrio) fijo + doble linea del Master
        auto fp = faderPanelBounds.toFloat();
        if (! fp.isEmpty())
        {
            juce::ColourGradient grad (juce::Colour (0x16ffffff), fp.getX(), fp.getY(),
                                       juce::Colour (0x05ffffff), fp.getX(), fp.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (fp, 12.0f);
            g.setColour (juce::Colour (0x22ffffff));
            g.drawRoundedRectangle (fp, 12.0f, 1.0f);
            g.setColour (juce::Colour (0x14ffffff));
            g.drawLine (fp.getX() + 12.0f, fp.getY() + 1.5f, fp.getRight() - 12.0f, fp.getY() + 1.5f, 1.0f);

            if (masterSepX > 0)   // separador doble entre faders y la columna fija (Buses/MIDI/etc)
            {
                auto fpr = fp.reduced (0.0f, 14.0f);
                auto dl = [&] (float sx)
                {
                    g.setColour (juce::Colour (0x99202020));
                    g.fillRoundedRectangle (sx - 2.5f, fpr.getY(), 5.0f, fpr.getHeight(), 2.5f);
                    g.setColour (juce::Colour (0x44ffffff));
                    g.fillRoundedRectangle (sx - 1.0f, fpr.getY(), 2.0f, fpr.getHeight(), 1.0f);
                };
                dl ((float) masterSepX - 4.5f); dl ((float) masterSepX + 4.5f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (midiMapMode && masterSlider.getBounds().contains (e.getPosition()))   // armar el master (MIDI)
        { armFaderIfMidi (-1, {}, true); return; }
        if (editMode && ! addClickBtnRect.isEmpty() && addClickBtnRect.contains (e.getPosition()))
        { agregarSeccionClick(); return; }               // #3 + agrega la sección de click
        if (editMode && ! delClickBtnRect.isEmpty() && delClickBtnRect.contains (e.getPosition()))
        { quitarSeccionClick(); return; }                // #3 − elimina la sección de click
        if (! mapBounds.contains (e.getPosition())) return;
        isDragging = true;
        dragSeeks = ! playing.load();
        dragStartCenter = browsing ? browseCenter : positionSeconds();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! isDragging) return;
        auto inner = mapBounds.reduced (8);
        const double win = juce::jmin (20.0, totalSeconds());
        const double dx = e.getDistanceFromDragStartX() * (win / juce::jmax (1, inner.getWidth())) * 0.5;   // menos sensible
        lastInteractionMs = juce::Time::getMillisecondCounter();
        if (dragSeeks) seekSeconds (dragStartCenter - dx);
        else { browsing = true; browseCenter = juce::jlimit (0.0, totalSeconds(), dragStartCenter - dx); repaint (mapBounds); }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isDragging) return;
        isDragging = false;
        if (dragSeeks)
        {
            if (e.getDistanceFromDragStart() < 5) seekFromMouse (e);          // clic simple
            else
            {                                                                // al soltar el arrastre, cae en un click
                auto inner = mapBounds.reduced (8);
                const double win = juce::jmin (20.0, totalSeconds());
                const double dx = e.getDistanceFromDragStartX() * (win / juce::jmax (1, inner.getWidth())) * 0.5;
                seekSeconds (snapToBeat (dragStartCenter - dx));
            }
        }
        else { browsing = false; repaint (mapBounds); }
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (stripBounds.contains (e.getPosition()))   // scroll horizontal de las tarjetas de canciones
        {
            double d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
            if (w.isReversed) d = -d;
            stripScroll = juce::jmax (0, stripScroll + (int) (d * 600.0));
            resized();
            return;
        }
        if (! mapBounds.contains (e.getPosition())) return;
        double d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        if (w.isReversed) d = -d;
        if (playing.load())
        {
            if (! browsing) browseCenter = positionSeconds();
            browsing = true;
            browseCenter = juce::jlimit (0.0, totalSeconds(), browseCenter + d * 14.0);
            lastInteractionMs = juce::Time::getMillisecondCounter();
            repaint (mapBounds);
        }
        else seekSeconds (positionSeconds() + d * 14.0);
    }

    // #4 teclas: en modo mapping asigna/desasigna; en modo normal dispara lo mapeado
    bool keyPressed (const juce::KeyPress& kp) override
    {
        const int code = kp.getKeyCode();
        if (keyMapMode)
        {
            if (armKind == 0) return false;   // nada armado
            // re-teclar la MISMA tecla ya asignada al elemento -> desasignar
            const bool same =
                (armKind == 1 && armedAct >= 0 && actKey[armedAct] == code)
             || (armKind == 2 && keyByTrack.count (armTrack)   && keyByTrack[armTrack]   == code)
             || (armKind == 4 && keyBySolo.count (armTrack)    && keyBySolo[armTrack]    == code)
             || (armKind == 5 && keyByBusMute.count (armTrack) && keyByBusMute[armTrack] == code)
             || (armKind == 6 && keyByBusSolo.count (armTrack) && keyByBusSolo[armTrack] == code)
             || (armKind == 3 && keyBySong.count (armSong)     && keyBySong[armSong]     == code);
            if (same)
            {
                if (armKind == 1) actKey[armedAct] = 0;
                else if (armKind == 2) keyByTrack[armTrack] = 0;
                else if (armKind == 4) keyBySolo[armTrack] = 0;
                else if (armKind == 5) keyByBusMute[armTrack] = 0;
                else if (armKind == 6) keyByBusSolo[armTrack] = 0;
                else if (armKind == 3) keyBySong[armSong] = 0;
            }
            else
            {
                clearAllForKey (code);   // sin duplicados: se quita de cualquier otro
                if (armKind == 1) actKey[armedAct] = code;
                else if (armKind == 2) keyByTrack[armTrack] = code;
                else if (armKind == 4) keyBySolo[armTrack] = code;
                else if (armKind == 5) keyByBusMute[armTrack] = code;
                else if (armKind == 6) keyByBusSolo[armTrack] = code;
                else if (armKind == 3) keyBySong[armSong] = code;
            }
            clearArm();
            saveKeyMap();
            repaint();
            return true;
        }
        // modo normal: disparar
        for (int a = 0; a < kaCount; ++a)
            if (actKey[a] != 0 && actKey[a] == code) { doAct (a); return true; }
        for (auto& kv : keyByTrack)
            if (kv.second == code) { const int idx = trackIndexForName (kv.first); if (idx >= 0) toggleTrackMute (idx); return true; }
        for (auto& kv : keyBySolo)
            if (kv.second == code) { const int idx = trackIndexForName (kv.first); if (idx >= 0) toggleTrackSolo (idx); return true; }
        for (auto& kv : keyByBusMute)
            if (kv.second == code) { const int f = familyNames.indexOf (kv.first); if (f >= 0) toggleBusMute (f); return true; }
        for (auto& kv : keyByBusSolo)
            if (kv.second == code) { const int f = familyNames.indexOf (kv.first); if (f >= 0) toggleBusSolo (f); return true; }
        for (auto& kv : keyBySong)
            if (kv.second == code) { selectSongById (kv.first); return true; }
        return false;
    }
    void toggleTrackSolo (int idx)
    {
        if (idx < 0 || idx >= kMaxTracks || idx >= trackSliders.size()) return;
        const bool on = ! trackSolo[idx].load();
        trackSolo[idx].store (on);
        if (idx < soloDots.size()) { soloDots[idx]->on = on; soloDots[idx]->repaint(); }
        markDirty();
    }
    int trackIndexForName (const juce::String& name) const
    {
        for (int i = 0; i < trackNames.size(); ++i) if (trackNames[i] == name) return i;
        return -1;
    }
    void toggleTrackMute (int idx)
    {
        if (idx < 0 || idx >= trackSliders.size() || idx >= trackLabels.size()) return;
        const int fam = trackFamily[idx];
        const bool famLocked = ! mixPerSong && fam < familyNames.size() && globalMutedFamilies.count (familyNames[fam]) > 0;
        bool m = ! trackMuted[idx].load();
        if (famLocked) m = true;   // regla: no se puede desmutear un canal si su familia (bus) está muteada
        trackMuted[idx].store (m);
        trackLabels[idx]->setColour (juce::Label::textColourId, m ? juce::Colour (0xffe05555) : juce::Colour (0xfff2f2f2));
        trackLabels[idx]->repaint();
        trackSliders[idx]->getProperties().set ("muted", m);
        trackSliders[idx]->repaint();
        markDirty();
        if (! mixPerSong && ! suppressGlobalSave && ! famLocked && idx < trackNames.size())   // canal suelto en la mezcla general
        {
            if (m) globalMuted.insert (trackNames[idx]); else globalMuted.erase (trackNames[idx]);
            saveStorageCfg();
        }
    }
    void selectSongById (int songId)
    {
        for (int i = 0; i < repertoire.size(); ++i)
            if (repertoire.getReference (i).id == songId) { if (i < songReady.size() && ! songReady[i]) return; loadSong (i); return; }
    }

    void drawKeyBadge (juce::Graphics& g, juce::Rectangle<int> host, int code, bool armd,
                       juce::Rectangle<int> clip = {})
    {
        if (! clip.isEmpty() && ! clip.contains (host.getCentre())) return;   // no pintar elementos a medio cortar
        // tinte translúcido sobre TODA el área mapeable (se ve el botón debajo)
        auto hf = host.toFloat().reduced (1.0f);
        const float rad = juce::jmin (6.0f, hf.getHeight() * 0.4f);
        g.setColour (armd ? juce::Colour (0x55F2B23A) : juce::Colour (0x330A84FF));
        g.fillRoundedRectangle (hf, rad);
        g.setColour (armd ? juce::Colour (0xccF2B23A) : juce::Colour (0x770A84FF));
        g.drawRoundedRectangle (hf, rad, 1.4f);
        // chip con la tecla en la esquina sup-derecha (se acomoda a controles chicos)
        const int w = juce::jmin (22, host.getWidth()  - 2);
        const int h = juce::jmin (16, host.getHeight() - 2);
        juce::Rectangle<int> chip (juce::jmax (host.getX() + 1, host.getRight() - w - 2), host.getY() + 1, w, h);
        g.setColour (armd ? juce::Colour (0xffF2B23A) : juce::Colour (0xfff2f2f2));
        g.fillRoundedRectangle (chip.toFloat(), 4.0f);
        g.setColour (juce::Colour (0xff141414)); g.setFont (juce::Font ((float) juce::jmin (11, h - 3) + 0.5f, juce::Font::bold));
        const juce::String lbl = armd ? juce::String::fromUTF8 ("\xe2\x80\xa6")
                                      : (code ? (badgeMidi ? midiLabel (code) : keyLabel (code)) : juce::String::fromUTF8 ("\xe2\x80\x94"));
        g.drawText (lbl, chip, juce::Justification::centred);
    }
    bool badgeMidi = false;   // los badges muestran MIDI (true) o tecla (false)

    void paintOverChildren (juce::Graphics& g) override
    {
        if (mixDirty && repertoireBtn.isVisible())   // puntito rojo: hay cambios de mezcla sin guardar
        {
            auto r = repertoireBtn.getBounds();
            const float d = 9.0f;
            juce::Rectangle<float> dot ((float) r.getRight() - d - 3.0f, (float) r.getY() + 3.0f, d, d);
            g.setColour (juce::Colour (0xff0a0a0a)); g.fillEllipse (dot.expanded (1.6f));
            g.setColour (juce::Colour (0xffE5484D)); g.fillEllipse (dot);
        }

        if (! keyMapMode && ! midiMapMode) return;   // badges sobre elementos mapeables (sin banner)
        badgeMidi = midiMapMode;
        auto codeOf = [this] (const std::map<juce::String,int>& km, const std::map<juce::String,int>& mm, const juce::String& n) -> int
        { auto& m = midiMapMode ? mm : km; auto it = m.find (n); return it != m.end() ? it->second : 0; };

        // botones fijos (transporte + Pad/Buses/MIDI/Faders)
        for (int a = 0; a < kaCount; ++a)
        {
            auto* b = btnForAct (a);
            if (b == nullptr || ! b->isVisible()) continue;
            const int code = midiMapMode ? (a < 16 ? actMidi[a] : 0) : actKey[a];
            drawKeyBadge (g, getLocalArea (b, b->getLocalBounds()), code, armKind == 1 && armedAct == a);
        }
        // mutes (nombre), solos (S) y —en MIDI— faders de cada track
        {
            const auto clip = faderViewport.isVisible() ? getLocalArea (&faderViewport, faderViewport.getLocalBounds())
                                                        : juce::Rectangle<int>();
            for (int i = 0; i < trackLabels.size() && i < trackNames.size(); ++i)
            {
                auto* l = trackLabels[i];
                if (l == nullptr || ! l->isShowing()) continue;
                const auto nm = trackNames[i];
                drawKeyBadge (g, getLocalArea (l, l->getLocalBounds()), codeOf (keyByTrack, midiTrackMute, nm), armKind == 2 && armTrack == nm, clip);
            }
            for (int i = 0; i < soloDots.size() && i < trackNames.size(); ++i)
            {
                auto* d = soloDots[i];
                if (d == nullptr || ! d->isShowing()) continue;
                const auto nm = trackNames[i];
                drawKeyBadge (g, getLocalArea (d, d->getLocalBounds()), codeOf (keyBySolo, midiTrackSolo, nm), armKind == 4 && armTrack == nm, clip);
            }
            if (midiMapMode)   // faders continuos (solo MIDI): sobre el slider
                for (int i = 0; i < trackSliders.size() && i < trackNames.size(); ++i)
                {
                    auto* s = trackSliders[i];
                    if (s == nullptr || ! s->isShowing()) continue;
                    const auto nm = trackNames[i];
                    const int fc = midiTrackFader.count (nm) ? midiTrackFader[nm] : 0;
                    drawKeyBadge (g, getLocalArea (s, s->getLocalBounds()), fc, armFader && armFaderIdx == i, clip);
                }
            // buses (familias): mute, solo y fader
            for (int f = 0; f < busLabels.size() && f < familyNames.size(); ++f)
            {
                auto* l = busLabels[f];
                if (l == nullptr || ! l->isShowing()) continue;
                const auto nm = familyNames[f];
                drawKeyBadge (g, getLocalArea (l, l->getLocalBounds()), codeOf (keyByBusMute, midiBusMute, nm), armKind == 5 && armTrack == nm, clip);
            }
            for (int f = 0; f < busSoloDots.size() && f < familyNames.size(); ++f)
            {
                auto* d = busSoloDots[f];
                if (d == nullptr || ! d->isShowing()) continue;
                const auto nm = familyNames[f];
                drawKeyBadge (g, getLocalArea (d, d->getLocalBounds()), codeOf (keyByBusSolo, midiBusSolo, nm), armKind == 6 && armTrack == nm, clip);
            }
            if (midiMapMode)
                for (int f = 0; f < busSliders.size() && f < familyNames.size(); ++f)
                {
                    auto* s = busSliders[f];
                    if (s == nullptr || ! s->isShowing()) continue;
                    const auto nm = familyNames[f];
                    const int fc = midiBusFader.count (nm) ? midiBusFader[nm] : 0;
                    drawKeyBadge (g, getLocalArea (s, s->getLocalBounds()), fc, armFader && armFaderIdx == -2 && armTrack == nm, clip);
                }
        }
        // master fader (solo MIDI)
        if (midiMapMode && masterSlider.isShowing())
            drawKeyBadge (g, getLocalArea (&masterSlider, masterSlider.getLocalBounds()), midiMasterFader, armFader && armFaderIdx == -1);
        // bloques de canción (tarjetas del repertorio)
        {
            const auto clip = getLocalArea (this, stripBounds);
            for (auto* c : songCards)
            {
                if (c == nullptr || ! c->isShowing()) continue;
                const int sid = c->songId;
                const int code = midiMapMode ? (midiSong.count (sid) ? midiSong[sid] : 0)
                                             : (keyBySong.count (sid) ? keyBySong[sid] : 0);
                auto full = getLocalArea (c, c->getLocalBounds());
                auto host = full.withTrimmedTop (6).withHeight ((int) ((full.getHeight() - 6) * 0.76));   // encierra la portada
                drawKeyBadge (g, host, code, armKind == 3 && armSong == sid, clip);
            }
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16);

        // Barra superior: logo (izq) | Play + Inicio (centro) | Conectar + tiempo (der)
        auto topbar = area.removeFromTop (46);
        {
            const int BW = 80, BH = 34, G = 8;
            const int gy = topbar.getCentreY() - BH / 2;

            // Izquierda (tras el logo): caja de tiempo + caja de tempo/compás + PAD
            const int logoW = 80, boxW = 64, boxG = 6;
            int lx = topbar.getX() + logoW;
            timeLabel.setBounds (lx, gy, boxW, BH);
            lx += boxW + boxG;
            compasBoxBounds = juce::Rectangle<int> (lx, gy, boxW, BH);
            lx += boxW + G;
            padBtn.setBounds (lx, gy, BW, BH);

            // Derecha: Editar, repertorios, configuraciones (misma medida)
            auto rr = topbar;
            settingsBtn.setBounds   (rr.removeFromRight (BW).withSizeKeepingCentre (BW, BH));
            rr.removeFromRight (G);
            repertoireBtn.setBounds (rr.removeFromRight (BW).withSizeKeepingCentre (BW, BH));
            rr.removeFromRight (G);
            editBtn.setBounds       (rr.removeFromRight (BW).withSizeKeepingCentre (BW, BH));

            // Centro: Inicio  Play  Fade (misma medida)
            const int cx = topbar.getCentreX();
            playButton.setBounds   (cx - BW / 2, gy, BW, BH);
            returnButton.setBounds (cx - BW / 2 - G - BW, gy, BW, BH);
            fadeButton.setBounds   (cx + BW / 2 + G, gy, BW, BH);
        }
        connStatus.setVisible (false);
        area.removeFromTop (6);

        // Franja con el nombre del repertorio centrado (solo si hay uno cargado)
        if (currentSetlistName.isNotEmpty()) { setlistBandBounds = area.removeFromTop (22); area.removeFromTop (3); }
        else                                   setlistBandBounds = {};

        // Tarjetas verticales grandes: portada arriba + nombre abajo (scroll horizontal)
        auto strip = area.removeFromTop (180);
        stripBounds = strip;
        {
            const int step = 250;
            const int n = songCards.size() + (editMode ? 1 : 0);
            const int contentW = juce::jmax (0, n * step - 10);
            stripScroll = juce::jlimit (0, juce::jmax (0, contentW - strip.getWidth()), stripScroll);
            int x = strip.getX() - stripScroll;
            for (auto* c : songCards) { c->setBounds (x, strip.getY(), 240, 178); x += step; }
            if (editMode) addCard.setBounds (x, strip.getY(), 240, 178);
        }
        area.removeFromTop (4);

        mapBounds = area.removeFromTop (188);
        {   // botones de navegación por compás, pegados a los bordes del mapa
            const int bw = 30, bh = 46, cy = mapBounds.getCentreY() - bh / 2;
            barPrevBtn.setBounds (mapBounds.getX() + 6, cy, bw, bh);
            barNextBtn.setBounds (mapBounds.getRight() - bw - 6, cy, bw, bh);
            const bool showNav = (currentSong >= 0);
            barPrevBtn.setVisible (showNav);
            barNextBtn.setVisible (showNav);
        }
        // #4 barra de Editar en el espacio entre el mapa y los faders (solo en edición)
        editBarBounds = {};
        if (editMode)
        {
            area.removeFromTop (5);
            auto bar = area.removeFromTop (34);
            editBarBounds = juce::Rectangle<int> (bar.getX(), bar.getY(), 400, bar.getHeight());
            auto b = editBarBounds;
            keyMapBtn.setBounds (b.removeFromLeft (188)); b.removeFromLeft (8);
            midiMapBtn.setBounds (b.removeFromLeft (150));
            keyMapBtn.toFront (false); midiMapBtn.toFront (false);
            area.removeFromTop (5);
        }
        else area.removeFromTop (8);
        faderPanelBounds = area;

        // Region FIJA a la derecha: separador doble + 6 botones + Master (no se desplazan)
        const int rightW = 20 + 62 + 10 + 78 + 8;
        auto fixed = area.removeFromRight (rightW);

        // Los tracks (solo esos) van en el viewport desplazable
        faderViewport.setBounds (area);
        midiPanel.setBounds (area);
        padPanel.setBounds (area);
        layoutFaderStrip();

        auto fx = fixed.reduced (0, 14);
        masterSepX = fixed.getX() + 10;      // doble linea (coords MainComponent)
        fx.removeFromLeft (20);
        auto btnCol = fx.removeFromLeft (62);
        fx.removeFromLeft (10);
        auto mcol = fx.removeFromRight (78);
        {
            juce::Button* btns[6] = { &faderViewBtn, &busesBtn, &padPlayerBtn, &muteMidiBtn, &repeatBtn, &infiniteBtn };
            const int bgap = 6;
            const int bh = (btnCol.getHeight() - bgap * 5) / 6;
            for (int i = 0; i < 6; ++i)
            {
                btns[i]->setBounds (btnCol.removeFromTop (bh));
                if (i < 5) btnCol.removeFromTop (bgap);
            }
        }
        masterLabel.setBounds (mcol.removeFromBottom (22));
        masterSlider.setBounds (mcol.reduced (6, 0));

        splash.setBounds (getLocalBounds());
        repPicker.setBounds (getLocalBounds());
        settingsPanel.setBounds (getLocalBounds());
        storagePanel.setBounds (getLocalBounds());
        audioCfg.setBounds (getLocalBounds());
        repEdit.setBounds (getLocalBounds());
    }

    void layoutFaderStrip()
    {
        const bool tv = (faderView == 0);
        auto& sl = tv ? trackSliders : busSliders;
        auto& lb = tv ? trackLabels  : busLabels;
        auto& dd = tv ? soloDots     : busSoloDots;
        const int nT = sl.size();

        juce::Array<int> order;
        if (tv) order = faderOrder;
        else    for (int i = 0; i < nT; ++i) order.add (i);

        const bool hasSep = tv && (numSpecialFaders > 0 && numSpecialFaders < nT);
        const int sepGap = hasSep ? 22 : 0;
        const int colGap = 10;
        const int colW   = 78;   // ancho comodo fijo por columna
        const int trackGaps = (nT >= 1) ? (hasSep ? sepGap + colGap * (nT - 2) : colGap * (nT - 1)) : 0;
        const int contentW = colW * juce::jmax (0, nT) + juce::jmax (0, trackGaps);

        const int vpW = faderViewport.getWidth();
        const int vpH = faderViewport.getHeight();
        const bool scroll = (contentW + 44 > vpW);
        const int stripW = juce::jmax (contentW + 44, vpW);
        const int stripH = juce::jmax (60, vpH - (scroll ? 12 : 0));
        faderStrip.setSize (stripW, stripH);

        auto lay = juce::Rectangle<int> (0, 0, stripW, stripH).reduced (22, 14);
        const int startX = lay.getX() + juce::jmax (0, (lay.getWidth() - contentW) / 2);
        juce::Rectangle<int> cols (startX, lay.getY(), contentW, lay.getHeight());

        faderSepX = -1;
        bool firstCol = true;
        for (int oi = 0; oi < order.size(); ++oi)
        {
            if (! firstCol)
            {
                if (hasSep && oi == numSpecialFaders) { auto gr = cols.removeFromLeft (sepGap); faderSepX = gr.getCentreX(); }
                else                                    cols.removeFromLeft (colGap);
            }
            firstCol = false;
            const int i = order[oi];
            auto col = cols.removeFromLeft (colW);
            lb[i]->setBounds (col.removeFromBottom (22));
            auto soloLane = col.removeFromLeft (16);
            dd[i]->setBounds (soloLane.reduced (0, 2));
            sl[i]->setBounds (col.reduced (6, 0));
        }
        faderStrip.repaint();
    }

    void paintFaderStrip (juce::Graphics& g)
    {
        if (faderSepX <= 0) return;
        auto fpr = faderStrip.getLocalBounds().toFloat().reduced (0.0f, 14.0f);
        g.setColour (juce::Colour (0x99202020));
        g.fillRoundedRectangle ((float) faderSepX - 2.5f, fpr.getY(), 5.0f, fpr.getHeight(), 2.5f);
        g.setColour (juce::Colour (0x44ffffff));
        g.fillRoundedRectangle ((float) faderSepX - 1.0f, fpr.getY(), 2.0f, fpr.getHeight(), 1.0f);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint (mapBounds); }

    void drawLogo (juce::Graphics& g, float x, float y)
    {
        if (logoImg.isValid())
        {
            const float hh = 45.0f;
            const float ww = hh * (float) logoImg.getWidth() / (float) juce::jmax (1, logoImg.getHeight());
            g.drawImage (logoImg, juce::Rectangle<float> (x, y - 2.0f, ww, hh), juce::RectanglePlacement::centred);
            return;
        }
        const float bw = 4.0f, gap = 3.5f, h = 30.0f;
        const float hs[6] = { 0.35f, 0.62f, 1.0f, 0.55f, 0.82f, 0.42f };
        g.setColour (juce::Colour (0xfff2f2f2));
        for (int i = 0; i < 6; ++i)
        {
            const float bh = h * hs[i];
            g.fillRoundedRectangle (x + i * (bw + gap), y + (h - bh), bw, bh, bw * 0.5f);
        }
        const float tx = x + 6 * (bw + gap) + 9;
        g.setColour (juce::Colour (0xfff2f2f2));
        g.setFont (juce::Font (13.0f));
        g.drawText ("Neural", (int) tx, (int) y - 1, 130, 15, juce::Justification::topLeft);
        g.setFont (juce::Font (14.5f, juce::Font::bold));
        g.drawText ("Play", (int) tx, (int) y + 13, 130, 16, juce::Justification::topLeft);
    }

    void loadConfig()
    {
        auto f = npAppDir().getChildFile ("config.json");
        if (! f.existsAsFile()) return;
        auto v = juce::JSON::parse (f.loadFileAsString());
        serverUrl   = v.getProperty ("serverUrl", "").toString();
        serverToken = v.getProperty ("token", "").toString();
        fetchPadPacks();
        loadCachedPerfiles();
        fetchPerfiles();
    }
    void loadCachedPerfiles()   // roster de perfiles cacheado (offline)
    {
        auto f = npAppDir().getChildFile ("perfiles.json");
        if (! f.existsAsFile()) return;
        auto txt = f.loadFileAsString().trim();
        if (txt.startsWithChar ('[')) { const juce::ScopedLock l (chartLock); perfilesJson = txt; }
    }
    void fetchPerfiles()   // baja el roster (nombre + acento + prefs) cuando hay internet
    {
        if (serverUrl.isEmpty() || serverToken.isEmpty()) return;
        const juce::String url = serverUrl + "/api/live/perfiles";
        const juce::String tok = serverToken;
        juce::Thread::launch ([this, url, tok]
        {
            auto txt = httpGet (url, tok).trim();
            if (txt.startsWithChar ('['))
            {
                { const juce::ScopedLock l (chartLock); perfilesJson = txt; }
                npAppDir().getChildFile ("perfiles.json").replaceWithText (txt);
            }
        });
    }

    // ── Pads: catálogo del servidor + selección ──────────────────────
    void fetchPadPacks()
    {
        if (serverUrl.isEmpty() || serverToken.isEmpty()) return;
        const juce::String url = serverUrl + "/api/live/pads";
        const juce::String tok = serverToken;
        juce::Thread::launch ([this, url, tok]
        {
            auto v = juce::JSON::parse (httpGet (url, tok));
            juce::Array<PadPack> packs;
            if (auto* arr = v.getArray())
                for (auto& e : *arr)
                {
                    PadPack pp;
                    pp.id      = e.getProperty ("id", "").toString();
                    pp.nombre  = e.getProperty ("nombre", "").toString();
                    pp.portada = e.getProperty ("portada", "").toString();
                    pp.baseIdx = (int) e.getProperty ("base_idx", 0);
                    pp.listo   = (bool) e.getProperty ("listo", false);
                    if (pp.id.isNotEmpty()) packs.add (pp);
                }
            juce::MessageManager::callAsync ([this, packs] { applyPadPacks (packs); });
        });
    }
    void applyPadPacks (const juce::Array<PadPack>& packs)
    {
        padPacks = packs;
        int sel = -1;
        for (int i = 0; i < padPacks.size(); ++i) if (padPacks[i].id == padPackId) { sel = i; break; }
        if (sel < 0 && ! padPacks.isEmpty()) sel = 0;
        if (sel >= 0) selectPadPack (padPacks.getReference (sel).id, false);
        updatePadUi();
    }
    void selectPadPack (const juce::String& id, bool userAction)
    {
        int idx = -1;
        for (int i = 0; i < padPacks.size(); ++i) if (padPacks[i].id == id) { idx = i; break; }
        if (idx < 0) return;
        const auto& pk = padPacks.getReference (idx);
        padPackId          = pk.id;
        padPackName        = pk.nombre;
        padPackBaseIdx     = pk.baseIdx;
        padPackPortadaRel  = pk.portada;
        padPortadaImg      = juce::Image();
        if (padPackPortadaRel.isNotEmpty())
        {
            const juce::String url = serverUrl + "/static/" + padPackPortadaRel;
            const juce::String tok = serverToken;
            const juce::File dest = npAppDir().getChildFile ("pads").getChildFile (padPackId)
                                        .getChildFile ("portada" + padPackPortadaRel.fromLastOccurrenceOf (".", true, false));
            juce::Thread::launch ([this, url, tok, dest]
            {
                if (! dest.existsAsFile()) { dest.getParentDirectory().createDirectory(); httpDownload (url, tok, dest); }
                auto img = juce::ImageFileFormat::loadFrom (dest);
                juce::MessageManager::callAsync ([this, img] { padPortadaImg = img; updatePadUi(); });
            });
        }
        computePadReadyMask();
        prefetchPadTones();      // baja los 12 tonos en segundo plano (para que el usuario vea el progreso)
        if (userAction)
        {
            saveStorageCfg();
            if (padEnabled.load()) padApplyTone();   // cambiar de pad re-suena el tono actual con el nuevo timbre
        }
        updatePadUi();
    }
    void updatePadUi()
    {
        const bool on = padEnabled.load();
        padBtn.setColour (juce::TextButton::buttonColourId, on ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff1f1f1f));
        padBtn.repaint();
        refreshPadPanel();
    }
    void refreshPadPanel()
    {
        padPanel.havePack   = ! padPackId.isEmpty();
        padPanel.packName   = padPackName;
        padPanel.portada    = padPortadaImg;
        padPanel.enabled    = padEnabled.load();
        padPanel.sel        = (padMode == 0) ? 0 : (padManualIdx + 1);
        padPanel.playingIdx = padPlayingIdx.load();
        padPanel.readyMask  = padReadyMask.load();
        padPanel.faderCc    = midiPadFader;
        padPanel.faderArmed = (armFader && armFaderIdx == -3);
        padPanel.fader.setValue (padGainDb, juce::dontSendNotification);
        padPanel.repaint();
    }
    void showPadPackMenu()
    {
        juce::PopupMenu menu;
        if (padPacks.isEmpty())
            menu.addItem (1, "(sin pads en Admin)", false, false);
        else
            for (int i = 0; i < padPacks.size(); ++i)
            {
                const auto& pk = padPacks.getReference (i);
                bool cached = true;
                for (int t = 0; t < 12; ++t) if (! padCacheFile (pk.id, t).existsAsFile()) { cached = false; break; }
                juce::String label = pk.nombre;
                if (! cached) label += "   (descargar)";
                menu.addItem (i + 1, label, true, pk.id == padPackId);
            }
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&padPanel),
            [this] (int r)
            {
                if (r > 0 && r <= padPacks.size())
                    selectPadPack (padPacks.getReference (r - 1).id, true);   // selecciona y dispara la descarga
            });
    }
    void armPadFader()
    {
        if (! midiMapMode) return;
        clearArm();
        armFader = true;
        armFaderIdx = -3;              // -3 = fader del pad
        refreshPadPanel();
        repaint();
    }
    void cyclePadPack (int dir)
    {
        if (padPacks.isEmpty()) return;
        int idx = 0;
        for (int i = 0; i < padPacks.size(); ++i) if (padPacks[i].id == padPackId) { idx = i; break; }
        idx = (idx + dir + padPacks.size()) % padPacks.size();
        selectPadPack (padPacks.getReference (idx).id, true);
    }
    void computePadReadyMask()
    {
        int m = 0;
        if (! padPackId.isEmpty())
            for (int i = 0; i < 12; ++i) if (padCacheFile (padPackId, i).existsAsFile()) m |= (1 << i);
        padReadyMask.store (m);
    }
    void prefetchPadTones()
    {
        if (padPackId.isEmpty()) return;
        const int gen = padPrefetchGen.fetch_add (1) + 1;
        const juce::String pack = padPackId;
        const juce::String tok  = serverToken;
        const juce::String base = serverUrl + "/api/live/pad/" + pack + "/";
        juce::Thread::launch ([this, gen, pack, tok, base]
        {
            for (int i = 0; i < 12; ++i)
            {
                if (padPrefetchGen.load() != gen) return;
                auto dest = padCacheFile (pack, i);
                if (! dest.existsAsFile())
                {
                    dest.getParentDirectory().createDirectory();
                    httpDownload (base + juce::String (i), tok, dest);
                }
                if (padPrefetchGen.load() != gen) return;
                juce::MessageManager::callAsync ([this, pack]
                {
                    if (padPackId == pack) { computePadReadyMask(); refreshPadPanel(); }
                });
            }
        });
    }

    void startLoad()
    {
        startLoadId (juce::String());
    }
    // ───────── Puente / Sincronizar ─────────
    void setSync (bool on)
    {
        syncEnabled = on;
        if (on)
        {
            liveServer.start();
            fetchLiveChartForCurrent();
            syncPing (false); syncPingCtr = 0;
            syncPoll();       syncPollCtr = 0;
        }
        else
        {
            liveServer.stop();
            syncPing (true);
            syncLinked.store (false);
        }
        settingsPanel.setState (syncEnabled, syncLinked.load());
    }

    void fetchLiveChartForCurrent()
    {
        if (currentSong >= 0 && currentSong < repertoire.size())
            fetchLiveChart (repertoire.getReference (currentSong).id, repertoire.getReference (currentSong).tono);
    }

    void fetchLiveChart (int songId, int tono)
    {
        if (serverUrl.isEmpty() || serverToken.isEmpty()) return;
        const auto url = serverUrl; const auto tok = serverToken;
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, url, tok, songId, tono]
        {
            auto js = httpGet (url + "/api/live/chart/" + juce::String (songId) + "?t=" + juce::String (tono), tok);
            juce::MessageManager::callAsync ([sp, js]
            {
                if (sp == nullptr) return;
                { const juce::ScopedLock l (sp->chartLock);
                  sp->currentChartJson = js.isNotEmpty() ? js : juce::String ("{}"); }
                sp->liveSongVer.fetch_add (1);   // avisa a los músicos que recarguen /song
            });
        });
    }

    int liveSectionIndexAt (double pos) const
    {
        if (sectionTimes.isEmpty()) return 0;
        int idx = 0;
        for (int i = 0; i < sectionTimes.size(); ++i)
        { if (pos >= sectionTimes[i]) idx = i; else break; }
        return idx;
    }

    // ───────── Salida de audio: dispositivo + enrutamiento por familia ─────────
    juce::StringArray outputDeviceNames()
    {
        juce::StringArray names;
        const auto& types = deviceManager.getAvailableDeviceTypes();
        for (auto* t : types) { t->scanForDevices(); names.addArray (t->getDeviceNames (false)); }
        return names;
    }

    juce::String currentDeviceName()
    {
        if (auto* d = deviceManager.getCurrentAudioDevice()) return d->getName();
        return {};
    }

    int currentOutputChannelCount()
    {
        if (auto* d = deviceManager.getCurrentAudioDevice())
        {
            int n = d->getActiveOutputChannels().countNumberOfSetBits();
            if (n <= 0) n = d->getOutputChannelNames().size();
            return n;
        }
        return 2;
    }

    void ensureDeviceRoutes (const juce::String& name)
    {
        if (name.isEmpty()) return;
        if (routesByDevice.find (name) == routesByDevice.end())
        {
            std::array<AudioConfigPanel::FamRoute, kNumFam> def;
            for (auto& r : def) { r.mode = 2; r.ch = 0; }   // por defecto estéreo 1/2
            routesByDevice[name] = def;
        }
    }

    juce::Array<double> deviceSampleRates()
    {
        if (auto* dev = deviceManager.getCurrentAudioDevice()) return dev->getAvailableSampleRates();
        return {};
    }
    double currentDeviceSampleRate()
    {
        if (auto* dev = deviceManager.getCurrentAudioDevice()) return dev->getCurrentSampleRate();
        return deviceSampleRate;
    }

    void applyAudioDevice (const juce::String& name)
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName = name;
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        setup.outputChannels.setRange (0, 32, true);
        setup.bufferSize = 256;
        setup.sampleRate = preferredSampleRate;   // 0 = automático (sigue la frecuencia del dispositivo)
        deviceManager.setAudioDeviceSetup (setup, true);
        audioOutDevice = name;
        openOutChans = juce::jlimit (1, 32, currentOutputChannelCount());
        ensureDeviceRoutes (name);
        snapshotRoutes();
        audioCfg.buildRouteItems (openOutChans);
        audioCfg.setSampleRates (deviceSampleRates(), currentDeviceSampleRate(), preferredSampleRate);
        applyRoutesToUI();
        saveAudioRouting();
    }

    void setFamRoute (int fam, int mode, int base)
    {
        if (audioOutDevice.isEmpty()) audioOutDevice = currentDeviceName();
        ensureDeviceRoutes (audioOutDevice);
        auto& arr = routesByDevice[audioOutDevice];
        if (fam >= 0 && fam < kNumFam) { arr[fam].mode = mode; arr[fam].ch = base; }
        snapshotRoutes();
        saveAudioRouting();
    }

    void snapshotRoutes()
    {
        const juce::ScopedLock sl (graphLock);
        std::array<AudioConfigPanel::FamRoute, kNumFam> arr;
        auto it = routesByDevice.find (audioOutDevice);
        if (it != routesByDevice.end()) arr = it->second;
        else for (auto& r : arr) { r.mode = 2; r.ch = 0; }
        for (int i = 0; i < kNumFam; ++i)
        {
            int mode = arr[i].mode, base = arr[i].ch;
            if (mode == 2 && base + 1 >= openOutChans) mode = (base < openOutChans ? 1 : 0);
            if (mode == 1 && base >= openOutChans)      mode = 0;
            famMode[i]   = mode;
            famBaseCh[i] = base;
        }
    }

    void applyRoutesToUI()
    {
        auto it = routesByDevice.find (audioOutDevice);
        if (it == routesByDevice.end()) { for (int i = 0; i < kNumFam; ++i) audioCfg.setRoute (i, 2, 0); return; }
        for (int i = 0; i < kNumFam; ++i) audioCfg.setRoute (i, it->second[i].mode, it->second[i].ch);
    }

    void openAudioConfig()
    {
        if (audioOutDevice.isEmpty())
        {
            audioOutDevice = currentDeviceName();
            ensureDeviceRoutes (audioOutDevice);
            openOutChans = juce::jlimit (1, 32, currentOutputChannelCount());
            snapshotRoutes();
        }
        audioCfg.setBounds (getLocalBounds());
        audioCfg.setDevices (outputDeviceNames(), audioOutDevice);
        audioCfg.buildRouteItems (openOutChans);
        audioCfg.setSampleRates (deviceSampleRates(), currentDeviceSampleRate(), preferredSampleRate);
        applyRoutesToUI();
        audioCfg.setVisible (true);
        audioCfg.toFront (true);
    }

    void computeTrackRouteFam()
    {
        for (int i = 0; i < kMaxTracks; ++i) trackRouteFam[i] = 10;
        const int nt = juce::jmin (numTracks, (int) kMaxTracks);
        for (int i = 0; i < nt; ++i)
        {
            juce::String sf = (i < trackServerFam.size() ? trackServerFam[i] : juce::String());
            juce::String tn = (i < trackNames.size()     ? trackNames[i]     : juce::String());
            trackRouteFam[i] = routeFamIndex (sf, tn);
        }
    }

    void saveAudioRouting()
    {
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty ("device", audioOutDevice);
        root->setProperty ("sample_rate", preferredSampleRate);
        juce::DynamicObject::Ptr dev = new juce::DynamicObject();
        for (auto& kv : routesByDevice)
        {
            juce::Array<juce::var> arr;
            for (auto& r : kv.second)
            {
                juce::DynamicObject::Ptr o = new juce::DynamicObject();
                o->setProperty ("mode", r.mode);
                o->setProperty ("ch", r.ch);
                arr.add (juce::var (o.get()));
            }
            dev->setProperty (kv.first, arr);
        }
        root->setProperty ("routes", juce::var (dev.get()));
        npAppDir().getChildFile ("audio_routing.json").replaceWithText (juce::JSON::toString (juce::var (root.get())));
    }

    void loadAudioRouting()
    {
        auto f = npAppDir().getChildFile ("audio_routing.json");
        if (! f.existsAsFile()) return;
        auto v = juce::JSON::parse (f.loadFileAsString());
        if (! v.isObject()) return;
        audioOutDevice = v.getProperty ("device", "").toString();
        preferredSampleRate = (double) v.getProperty ("sample_rate", 0.0);
        auto routes = v.getProperty ("routes", juce::var());
        if (auto* obj = routes.getDynamicObject())
            for (auto& p : obj->getProperties())
            {
                std::array<AudioConfigPanel::FamRoute, kNumFam> arr;
                for (auto& r : arr) { r.mode = 2; r.ch = 0; }
                if (auto* a = p.value.getArray())
                    for (int i = 0; i < juce::jmin ((int) a->size(), kNumFam); ++i)
                    {
                        arr[i].mode = (int) (*a)[i].getProperty ("mode", 2);
                        arr[i].ch   = (int) (*a)[i].getProperty ("ch", 0);
                    }
                routesByDevice[p.name.toString()] = arr;
            }
    }

    void syncPing (bool bye)
    {
        if (serverUrl.isEmpty() || serverToken.isEmpty()) return;
        const auto url = serverUrl; const auto tok = serverToken;
        const juce::String accion = bye ? "bye" : "ping";
        juce::Thread::launch ([url, tok, accion]
        {
            juce::String body = "{\"token\":\"" + tok + "\",\"ip\":\""
                                + localLanIp() + "\",\"accion\":\"" + accion + "\"}";
            httpPostJson (url + "/api/live_ping", body);
        });
    }

    void syncPoll()
    {
        if (serverUrl.isEmpty()) return;
        juce::Component::SafePointer<MainComponent> sp (this);
        const auto url = serverUrl;
        juce::Thread::launch ([sp, url]
        {
            auto v = juce::JSON::parse (httpGet (url + "/api/live_status", {}));
            const bool activo = (bool) v.getProperty ("activo", false);
            juce::MessageManager::callAsync ([sp, activo]
            {
                if (sp == nullptr) return;
                sp->syncLinked.store (activo);
                if (sp->settingsPanel.isVisible())
                    sp->settingsPanel.setState (sp->syncEnabled, activo);
            });
        });
    }

    void reloadCurrent()   // "Actualizar": vuelve a bajar el setlist actual (agarra cambios de tono/chart)
    {
        startLoadId (lastSetlistId);
    }

    // ───────── Edición de repertorio ─────────
    void refreshEditAvailability()   // sin repertorio cargado no se puede editar
    {
        const bool hay = ! lastSetlistId.isEmpty();
        editBtn.setEnabled (hay);
        editBtn.setAlpha (hay ? 1.0f : 0.45f);
        if (! hay && editMode) toggleEdit();   // si quedó sin repertorio estando en edición, salir
    }

    void updateTransportEnabled()   // Play/Inicio/Fade: off en edición, PERO on en modo mapping (para poder armarlos)
    {
        const bool en = ! editMode || keyMapMode || midiMapMode;
        playButton.setEnabled (en);
        returnButton.setEnabled (en);
        fadeButton.setEnabled (en);
    }
    void toggleEdit()
    {
        if (! editMode && lastSetlistId.isEmpty()) return;   // no entrar en edición sin repertorio
        editMode = ! editMode;
        editBtn.setColour (juce::TextButton::buttonColourId, editMode ? juce::Colour (0xff2E6BE6) : juce::Colour (0xff1f1f1f));
        editBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        editBtn.repaint();
        if (editMode) playing.store (false);          // en edición no se reproduce
        updateTransportEnabled();
        keyMapBtn.setVisible (editMode);              // #4 botones de mapping solo en la barra de Editar
        midiMapBtn.setVisible (editMode);
        if (! editMode && (keyMapMode || midiMapMode))   // al salir de edición, salir de los mappings
        {
            keyMapMode = false; midiMapMode = false; clearArm(); setFadersArmable (false);
            refreshMapButtons();
        }
        resized();
        rebuildRepertoireStrip();
        repaint();   // #4 limpiar badges de mapping que hayan quedado pintados
    }

    void postThenReload (juce::String baseUrl, juce::StringPairArray params)
    {
        const auto tok = serverToken;
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, baseUrl, params, tok]
        {
            httpPostForm (baseUrl, params, tok);
            juce::MessageManager::callAsync ([sp] { if (sp) sp->reloadCurrent(); });
        });
    }

    void removeSong (int songId)
    {
        if (lastSetlistId.isEmpty() || serverUrl.isEmpty()) return;
        for (int i = 0; i < repertoire.size(); ++i)   // UI optimista: la tarjeta desaparece al instante
            if (repertoire.getReference (i).id == songId)
            {
                repertoire.remove (i);
                if (i < songMaster.size())   songMaster.remove (i);
                if (i < songMixCache.size()) songMixCache.remove (i);
                if (i < songReady.size())    songReady.remove (i);
                if (currentSong == i) { currentSong = -1; clearSong(); }
                else if (currentSong > i) --currentSong;
                break;
            }
        rebuildRepertoireStrip();
        postThenReload (serverUrl + "/api/live/setlist/" + lastSetlistId + "/quitar/" + juce::String (songId), {});
    }
    void setSongTono (int songId, juce::String tonoName)
    {
        if (lastSetlistId.isEmpty() || serverUrl.isEmpty()) return;
        juce::StringPairArray p; p.set ("tono", tonoName);
        postThenReload (serverUrl + "/api/live/setlist/" + lastSetlistId + "/tono/" + juce::String (songId), p);
    }
    void addSong (int songId, juce::String tonoName)
    {
        if (lastSetlistId.isEmpty() || serverUrl.isEmpty()) return;
        const int afterId = pendingAddAfterId; pendingAddAfterId = 0;   // 0 = al final; si no, tras esa canción
        SongEntry ph;                                   // tarjeta placeholder inmediata
        ph.id = -songId;
        ph.titulo = juce::String::fromUTF8 ("Agregando\xe2\x80\xa6");
        ph.tonoNombre = tonoName;
        int at = repertoire.size();
        if (afterId != 0)
            for (int i = 0; i < repertoire.size(); ++i)
                if (repertoire.getReference (i).id == afterId) { at = i + 1; break; }
        repertoire.insert (at, ph);
        songMaster.insert (at, 0.0);
        songMixCache.insert (at, juce::var());
        songReady.insert (at, false);
        rebuildRepertoireStrip();
        if (afterId == 0) { stripScroll = 1000000; resized(); }         // al final: desliza para mostrarla
        juce::StringPairArray p; p.set ("numero", juce::String (songId)); p.set ("tono", tonoName);
        if (afterId != 0) p.set ("after", juce::String (afterId));
        postThenReload (serverUrl + "/api/live/setlist/" + lastSetlistId + "/agregar", p);
    }

    // ───────── Miniaturas de portada en la biblioteca (#7) ─────────
    std::map<int, juce::Image> bibCoverCache;
    void loadBibCovers (juce::Array<RepEditPanel::BibItem> items)
    {
        for (auto& it : items)                          // aplica al instante las que ya estén en caché
        {
            auto f = bibCoverCache.find (it.id);
            if (f != bibCoverCache.end()) repEdit.setBibCover (it.id, f->second);
        }
        const auto url = serverUrl, tok = serverToken;
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, items, url, tok]
        {
            for (auto& it : items)
            {
                if (it.portada.isEmpty()) continue;
                auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("np_bibcov_" + juce::String (it.id) + ".img");
                if (! tmp.existsAsFile() || tmp.getSize() < 300)
                    httpDownload (url + "/static/" + it.portada, tok, tmp);
                auto img = juce::ImageFileFormat::loadFrom (tmp);
                if (! img.isValid()) continue;
                auto small = img.rescaled (72, 72, juce::Graphics::mediumResamplingQuality);
                const int id = it.id;
                juce::MessageManager::callAsync ([sp, id, small]
                {
                    if (sp == nullptr) return;
                    sp->bibCoverCache[id] = small;
                    sp->repEdit.setBibCover (id, small);
                });
            }
        });
    }

    // ───────── Guardar/aplicar mezcla por canción ─────────
    juce::var buildMixVar()   // estado actual del mezclador -> var
    {
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty ("master", masterSlider.getValue());
        juce::Array<juce::var> tr;
        for (int i = 0; i < trackSliders.size(); ++i)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("n", i < trackNames.size() ? trackNames[i] : juce::String());
            o->setProperty ("g", trackSliders[i]->getValue());
            o->setProperty ("m", trackMuted[i].load());
            o->setProperty ("s", trackSolo[i].load());
            tr.add (juce::var (o.get()));
        }
        root->setProperty ("tracks", tr);
        juce::Array<juce::var> bu;
        for (int f = 0; f < busSliders.size(); ++f)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("n", f < familyNames.size() ? familyNames[f] : juce::String());
            o->setProperty ("g", busSliders[f]->getValue());
            bu.add (juce::var (o.get()));
        }
        root->setProperty ("buses", bu);
        return juce::var (root.get());
    }

    // Cambios de mezcla sin guardar → puntito rojo en Repertorios
    void markDirty()
    {
        if (suppressGlobalSave) return;                 // cambio programático (cargar/aplicar), no cuenta
        if (! mixDirty) { mixDirty = true; repaint (repertoireBtn.getBounds().expanded (6)); }
    }
    void setMixSaved()
    {
        mixDirty = false;
        repPicker.dirty = false; repPicker.repaint();    // sincronizar el aviso del picker
        repaint (repertoireBtn.getBounds().expanded (6));
    }

    // Guarda la mezcla de la canción actual en la caché en memoria (por repertorio)
    void snapshotCurrentMix()
    {
        if (currentSong >= 0 && currentSong < songMixCache.size() && ! trackSliders.isEmpty())
            songMixCache.set (currentSong, buildMixVar());
    }

    // Guarda en el servidor la mezcla de TODAS las canciones del repertorio cargado
    void saveRepertoireMixes (juce::String sid)
    {
        if (sid.isEmpty() || serverUrl.isEmpty()) return;
        setMixSaved();                                   // se apaga el puntito rojo
        snapshotCurrentMix();
        juce::String data ("{");
        bool first = true;
        for (int i = 0; i < repertoire.size(); ++i)
            if (i < songMixCache.size() && songMixCache[i].isObject())
            {
                if (! first) data << ",";
                first = false;
                data << "\"" << repertoire.getReference (i).id << "\":" << juce::JSON::toString (songMixCache[i]);
            }
        data << "}";
        const auto base = serverUrl + "/api/live/setlist/" + sid + "/mix";
        const auto tok = serverToken;
        juce::StringPairArray p; p.set ("data", data);
        juce::Thread::launch ([base, p, tok] { httpPostForm (base, p, tok); });
    }

    void applyMix (const juce::var& mix)
    {
        const bool prevSup = suppressGlobalSave; suppressGlobalSave = true;   // no pisar la mezcla general
        if (masterPerSong && mix.hasProperty ("master"))                 // master solo si es por canción
            masterSlider.setValue ((double) mix.getProperty ("master", 0.0), juce::sendNotificationSync);
        if (auto* tr = mix.getProperty ("tracks", juce::var()).getArray())
            for (auto& t : *tr)
            {
                const int i = trackNames.indexOf (t.getProperty ("n", "").toString());
                if (i < 0 || i >= trackSliders.size()) continue;
                trackSliders[i]->setValue ((double) t.getProperty ("g", 0.0), juce::sendNotificationSync);   // fader: siempre por canción
                const bool m = (bool) t.getProperty ("m", false), s = (bool) t.getProperty ("s", false);
                trackMuted[i].store (m);
                trackLabels[i]->setColour (juce::Label::textColourId, m ? juce::Colour (0xffe05555) : juce::Colour (0xfff2f2f2));
                trackLabels[i]->repaint();
                trackSliders[i]->getProperties().set ("muted", m); trackSliders[i]->repaint();
                trackSolo[i].store (s); if (i < soloDots.size()) { soloDots[i]->on = s; soloDots[i]->repaint(); }
            }
        if (auto* bu = mix.getProperty ("buses", juce::var()).getArray())
            for (auto& b : *bu)
            {
                const int f = familyNames.indexOf (b.getProperty ("n", "").toString());
                if (f >= 0 && f < busSliders.size())
                    busSliders[f]->setValue ((double) b.getProperty ("g", 0.0), juce::sendNotificationSync);
            }
        suppressGlobalSave = prevSup;
        applyGlobalOverrides();   // si buses/mute son generales, ganan sobre lo de la canción
    }

    void snapshotGlobalFromCurrent()   // toma la mezcla actual (buses+mute) como base de la general
    {
        globalBusGain.clear(); globalMuted.clear(); globalMutedFamilies.clear();
        for (int f = 0; f < busSliders.size() && f < familyNames.size(); ++f)
        {
            globalBusGain[familyNames[f]] = busSliders[f]->getValue();
            int cnt = 0; bool allM = true;
            for (int i = 0; i < trackSliders.size(); ++i)
                if (trackFamily[i] == f) { ++cnt; if (! trackMuted[i].load()) allM = false; }
            if (cnt > 0 && allM) globalMutedFamilies.insert (familyNames[f]);   // familia completa muteada = bus muteado
        }
        for (int i = 0; i < trackSliders.size() && i < trackNames.size(); ++i)   // canales sueltos (familia no muteada completa)
        {
            const int fam = trackFamily[i];
            const bool famMute = fam < familyNames.size() && globalMutedFamilies.count (familyNames[fam]) > 0;
            if (trackMuted[i].load() && ! famMute) globalMuted.insert (trackNames[i]);
        }
        saveStorageCfg();
    }
    void applyGlobalOverrides()   // mezcla general de buses + mute (cuando mixPerSong = false)
    {
        if (mixPerSong) return;
        const bool prevSup = suppressGlobalSave; suppressGlobalSave = true;
        for (int i = 0; i < trackSliders.size() && i < trackNames.size(); ++i)
        {
            const int fam = trackFamily[i];
            const bool famMute = fam < familyNames.size() && globalMutedFamilies.count (familyNames[fam]) > 0;
            const bool m = famMute || globalMuted.count (trackNames[i]) > 0;   // familia (bus) O canal suelto
            trackMuted[i].store (m);
            trackLabels[i]->setColour (juce::Label::textColourId, m ? juce::Colour (0xffe05555) : juce::Colour (0xfff2f2f2));
            trackLabels[i]->repaint();
            trackSliders[i]->getProperties().set ("muted", m); trackSliders[i]->repaint();
        }
        for (int f = 0; f < busSliders.size() && f < familyNames.size(); ++f)
        {
            auto it = globalBusGain.find (familyNames[f]);
            busSliders[f]->setValue (it != globalBusGain.end() ? it->second : 0.0, juce::sendNotificationSync);
        }
        refreshBusStates();
        suppressGlobalSave = prevSup;
    }

    void createSetlist()
    {
        const auto now = juce::Time::getCurrentTime();
        const auto hoy = juce::String::formatted ("%04d-%02d-%02d",
                                                  now.getYear(), now.getMonth() + 1, now.getDayOfMonth());
        auto* aw = new juce::AlertWindow (juce::String::fromUTF8 ("Nuevo repertorio"),
                                          juce::String::fromUTF8 ("Nombre y fecha del repertorio:"),
                                          juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("n", "", juce::String::fromUTF8 ("Nombre:"));
        aw->addTextEditor ("f", hoy, juce::String::fromUTF8 ("Fecha (AAAA-MM-DD):"));
        aw->addButton ("Crear", 1);
        aw->addButton ("Cancelar", 0);
        juce::Component::SafePointer<MainComponent> sp (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([sp, aw] (int r)
        {
            const juce::String nombre = aw->getTextEditorContents ("n").trim();
            const juce::String fecha  = aw->getTextEditorContents ("f").trim();
            if (r == 1 && sp != nullptr)
                sp->doCreateSetlist (nombre.isNotEmpty() ? nombre : juce::String ("Nuevo repertorio"), fecha);
        }), true);
    }

    void doCreateSetlist (juce::String nombre, juce::String fecha)
    {
        if (serverUrl.isEmpty()) return;
        const auto base = serverUrl + "/api/live/setlist/crear"; const auto tok = serverToken;
        juce::StringPairArray p; p.set ("nombre", nombre);
        if (fecha.isNotEmpty()) p.set ("fecha", fecha);
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, base, p, tok]
        {
            auto v = juce::JSON::parse (httpPostForm (base, p, tok));
            const auto id = v.getProperty ("id", "").toString();
            juce::MessageManager::callAsync ([sp, id]
            {
                if (sp == nullptr || id.isEmpty()) return;
                if (! sp->editMode) sp->toggleEdit();   // entrar en modo edición para agregar canciones
                sp->startLoadId (id);
            });
        });
    }

    void confirmDeleteSetlist (juce::String id)
    {
        juce::String nombre = id;
        for (auto& it : repPicker.items) if (it.id == id) { nombre = it.nombre; break; }
        auto* aw = new juce::AlertWindow (juce::String::fromUTF8 ("Borrar repertorio"),
                                          juce::String::fromUTF8 ("\xc2\xbf" "Borrar \"") + nombre
                                              + juce::String::fromUTF8 ("\"? No se puede deshacer."),
                                          juce::MessageBoxIconType::NoIcon);
        aw->addButton ("Borrar", 1);
        aw->addButton ("Cancelar", 0);
        juce::Component::SafePointer<MainComponent> sp (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([sp, id] (int r)
        {
            if (r == 1 && sp != nullptr) sp->doDeleteSetlist (id);
        }), true);
    }

    void doDeleteSetlist (juce::String id)
    {
        if (serverUrl.isEmpty()) return;
        const auto base = serverUrl + "/api/live/setlist/" + id + "/eliminar";
        const auto tok = serverToken;
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, base, tok, id]
        {
            httpPostForm (base, {}, tok);
            juce::MessageManager::callAsync ([sp, id]
            {
                if (sp == nullptr) return;
                if (sp->lastSetlistId == id)   // borramos el que estaba cargado: limpiar
                {
                    sp->lastSetlistId.clear();
                    sp->currentSetlistName.clear();
                    sp->repertoire.clearQuick();
                    sp->rebuildRepertoireStrip();
                    sp->clearSong();
                    sp->refreshEditAvailability();
                }
                if (sp->repPicker.isVisible()) sp->openRepertoirePicker();   // refrescar la lista
            });
        });
    }

    void openBibliotecaForAddAfter (int afterId) { pendingAddAfterId = afterId; openBibliotecaForAdd(); }

    void openBibliotecaForAdd()
    {
        if (lastSetlistId.isEmpty() || serverUrl.isEmpty()) return;
        repEdit.serverUrl = serverUrl; repEdit.token = serverToken;
        repEdit.setBounds (getLocalBounds());
        const auto url = serverUrl; const auto tok = serverToken;
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, url, tok]
        {
            auto v = juce::JSON::parse (httpGet (url + "/api/live/setlists", tok));
            juce::Array<RepEditPanel::BibItem> items;
            if (auto* obj = v.getProperty ("canciones", juce::var()).getDynamicObject())
                for (auto& pr : obj->getProperties())
                {
                    RepEditPanel::BibItem it;
                    it.id     = (int) pr.value.getProperty ("id", 0);
                    it.titulo = pr.value.getProperty ("titulo", "").toString();
                    it.tono   = pr.value.getProperty ("tono", "").toString();
                    it.artista = pr.value.getProperty ("artista", "").toString();
                    it.portada = pr.value.getProperty ("portada", "").toString();
                    if (it.id > 0) items.add (it);
                }
            juce::MessageManager::callAsync ([sp, items]
            {
                if (sp == nullptr) return;
                sp->bibliotecaAll = items;
                sp->repEdit.openBiblioteca (items);
                sp->repEdit.setVisible (true); sp->repEdit.toFront (true);
            });
        });
    }

    void openTonoFor (int songId, juce::String title, bool addFlow)
    {
        if (serverUrl.isEmpty()) return;
        repEdit.serverUrl = serverUrl; repEdit.token = serverToken;
        repEdit.setBounds (getLocalBounds());
        const auto url = serverUrl; const auto tok = serverToken;
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, url, tok, songId, title, addFlow]
        {
            auto v = juce::JSON::parse (httpGet (url + "/api/live/tonos/" + juce::String (songId) + "?token=" + tok, tok));
            juce::Array<RepEditPanel::Key> ks;
            if (auto* a = v.getProperty ("keys", juce::var()).getArray())
                for (auto& kv : *a)
                {
                    RepEditPanel::Key k;
                    k.nombre   = kv.getProperty ("nombre", "").toString();
                    k.sem      = (int) kv.getProperty ("semitonos", 0);
                    k.rendered = (bool) kv.getProperty ("rendered", false);
                    ks.add (k);
                }
            juce::MessageManager::callAsync ([sp, songId, title, addFlow, ks]
            {
                if (sp == nullptr) return;
                double i2 = -1.0, o2 = -1.0;
                auto it = sp->songInOut.find (songId);
                if (it != sp->songInOut.end()) { i2 = it->second.first; o2 = it->second.second; }
                bool pIntro = false, pOutro = false;
                auto ip = sp->songPad.find (songId);
                if (ip != sp->songPad.end()) { pIntro = ip->second.first; pOutro = ip->second.second; }
                sp->repEdit.openTono (songId, title, addFlow, ks, i2, o2, pIntro, pOutro);
                sp->repEdit.setVisible (true); sp->repEdit.toFront (true);
            });
        });
    }

    // ───────── Almacenamiento / caché ─────────
    void loadStorageCfg()
    {
        auto v = juce::JSON::parse (npAppDir().getChildFile ("storage.json"));
        cacheAutoClean = (bool) v.getProperty ("auto", false);
        cacheCapGB = juce::jlimit (0, 500, (int) v.getProperty ("capGB", 0));
        countInEnabled = (bool) v.getProperty ("countin", false);
        masterPerSong = (bool) v.getProperty ("masterPerSong", true);
        globalMasterDb = (double) v.getProperty ("globalMaster", 0.0);
        mixPerSong = (bool) v.getProperty ("mixPerSong", true);
        globalBusGain.clear(); globalMuted.clear(); globalMutedFamilies.clear();
        if (auto* bg = v.getProperty ("globalBus", juce::var()).getDynamicObject())
            for (auto& pr : bg->getProperties()) globalBusGain[pr.name.toString()] = (double) pr.value;
        if (auto* gm = v.getProperty ("globalMuted", juce::var()).getArray())
            for (auto& e : *gm) globalMuted.insert (e.toString());
        if (auto* gf = v.getProperty ("globalMutedFam", juce::var()).getArray())
            for (auto& e : *gf) globalMutedFamilies.insert (e.toString());
        padPackId    = v.getProperty ("padPack", "").toString();
        padGainDb    = (double) v.getProperty ("padGainDb", 0.0);
        padGain.store (dbToGain ((float) padGainDb));
        padMode      = juce::jlimit (0, 1, (int) v.getProperty ("padMode", 0));
        padManualIdx = juce::jlimit (0, 11, (int) v.getProperty ("padManual", 0));
    }
    void saveStorageCfg()
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("auto", cacheAutoClean);
        o->setProperty ("capGB", cacheCapGB);
        o->setProperty ("countin", countInEnabled);
        o->setProperty ("masterPerSong", masterPerSong);
        o->setProperty ("globalMaster", globalMasterDb);
        o->setProperty ("mixPerSong", mixPerSong);
        juce::DynamicObject::Ptr bg = new juce::DynamicObject();
        for (auto& kv : globalBusGain) bg->setProperty (kv.first, kv.second);
        o->setProperty ("globalBus", juce::var (bg.get()));
        juce::Array<juce::var> gm; for (auto& n : globalMuted) gm.add (n);
        o->setProperty ("globalMuted", juce::var (gm));
        juce::Array<juce::var> gf; for (auto& n : globalMutedFamilies) gf.add (n);
        o->setProperty ("globalMutedFam", juce::var (gf));
        o->setProperty ("padPack", padPackId);
        o->setProperty ("padGainDb", padGainDb);
        o->setProperty ("padMode", padMode);
        o->setProperty ("padManual", padManualIdx);
        npAppDir().getChildFile ("storage.json").replaceWithText (juce::JSON::toString (juce::var (o.get())));
    }

    // #2 puntos de inicio/fin por canción (persistencia local)
    void loadInOut()
    {
        songInOut.clear();
        auto v = juce::JSON::parse (npAppDir().getChildFile ("inout.json"));
        if (auto* a = v.getArray())
            for (auto& e : *a)
            {
                const int id = (int) e.getProperty ("id", 0);
                if (id <= 0) continue;
                songInOut[id] = { (double) e.getProperty ("in", -1.0), (double) e.getProperty ("out", -1.0) };
            }
    }
    void saveInOut()
    {
        juce::Array<juce::var> a;
        for (auto& kv : songInOut)
        {
            if (kv.second.first < 0.0 && kv.second.second < 0.0) continue;
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("id", kv.first);
            o->setProperty ("in",  kv.second.first);
            o->setProperty ("out", kv.second.second);
            a.add (juce::var (o.get()));
        }
        npAppDir().getChildFile ("inout.json").replaceWithText (juce::JSON::toString (juce::var (a)));
    }
    void loadPadPlayer()
    {
        songPad.clear();
        auto v = juce::JSON::parse (npAppDir().getChildFile ("padplayer.json"));
        if (auto* a = v.getArray())
            for (auto& e : *a)
            {
                const int id = (int) e.getProperty ("id", 0);
                if (id <= 0) continue;
                songPad[id] = { (bool) e.getProperty ("intro", false), (bool) e.getProperty ("outro", false) };
            }
    }
    void savePadPlayer()
    {
        juce::Array<juce::var> a;
        for (auto& kv : songPad)
        {
            if (! kv.second.first && ! kv.second.second) continue;
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("id", kv.first);
            o->setProperty ("intro", kv.second.first);
            o->setProperty ("outro", kv.second.second);
            a.add (juce::var (o.get()));
        }
        npAppDir().getChildFile ("padplayer.json").replaceWithText (juce::JSON::toString (juce::var (a)));
    }
    void aplicarInOut (int songId)   // establece los atomics del reproductor para la canción actual
    {
        double inS = -1.0, outS = -1.0;
        auto it = songInOut.find (songId);
        if (it != songInOut.end()) { inS = it->second.first; outS = it->second.second; }
        songInSec.store (inS);
        songOutSec.store (outS);
        bool pi = false, po = false;
        auto ip = songPad.find (songId);
        if (ip != songPad.end()) { pi = ip->second.first; po = ip->second.second; }
        curPadIntro.store (pi);
        curPadOutro.store (po);
        padUserOverride = false;
        endPadAuto();     // reinicia la automatización al cambiar de canción
    }
    juce::Array<juce::File> cacheFolders() const
    {
        juce::Array<juce::File> out;
        for (auto& f : npCacheDir().findChildFiles (juce::File::findDirectories, false))
            if (f.getFileName().startsWith ("song_")) out.add (f);
        return out;
    }
    juce::StringArray activeCacheNames() const
    {
        juce::StringArray keep;
        for (auto& e : repertoire) keep.addIfNotAlreadyThere (e.folder.getFileName());
        return keep;
    }
    void withUsedFolders (std::function<void (juce::StringArray)> cb)
    {
        const auto url = serverUrl, tok = serverToken;
        const juce::StringArray activos = activeCacheNames();
        juce::Thread::launch ([url, tok, activos, cb]
        {
            juce::StringArray used = activos;
            auto v = juce::JSON::parse (httpGet (url + "/api/live/setlists", tok));
            if (auto* sls = v.getProperty ("setlists", juce::var()).getArray())
                for (auto& s : *sls)
                    if (auto* cs = s.getProperty ("canciones", juce::var()).getArray())
                        for (auto& c : *cs)
                        {
                            const int id = (int) c.getProperty ("id", 0);
                            const int tono = (int) c.getProperty ("tono_semitonos", 0);
                            used.addIfNotAlreadyThere ("song_" + juce::String (id) + "_t" + juce::String (tono));
                        }
            juce::MessageManager::callAsync ([cb, used] { cb (used); });
        });
    }
    void openStorage()
    {
        storagePanel.setBounds (getLocalBounds());
        storagePanel.setStats (0, 0, cacheAutoClean, cacheCapGB);
        storagePanel.setVisible (true);
        storagePanel.toFront (true);
        refreshStorageStats();
    }
    void refreshStorageStats()
    {
        juce::Component::SafePointer<MainComponent> sp (this);
        withUsedFolders ([sp] (juce::StringArray used)
        {
            if (sp == nullptr) return;
            juce::int64 total = 0, unused = 0;
            for (auto& f : sp->cacheFolders())
            {
                const auto sz = npFolderSize (f);
                total += sz;
                if (! used.contains (f.getFileName())) unused += sz;
            }
            sp->storagePanel.setStats (total, unused, sp->cacheAutoClean, sp->cacheCapGB);
        });
    }
    void deleteUnusedCache()
    {
        juce::Component::SafePointer<MainComponent> sp (this);
        withUsedFolders ([sp] (juce::StringArray used)
        {
            if (sp == nullptr) return;
            for (auto& f : sp->cacheFolders())
                if (! used.contains (f.getFileName())) f.deleteRecursively();
            sp->refreshStorageStats();
        });
    }
    void enforceCap()   // solo evita huérfanos: NUNCA borra audio que esté en algún repertorio (offline)
    {
        if (cacheCapGB <= 0) return;
        juce::Component::SafePointer<MainComponent> sp (this);
        withUsedFolders ([sp] (juce::StringArray used)
        {
            if (sp == nullptr || sp->cacheCapGB <= 0) return;
            const juce::int64 cap = (juce::int64) sp->cacheCapGB * 1073741824LL;
            std::vector<juce::File> fs;
            juce::int64 total = 0;
            for (auto& f : sp->cacheFolders()) { total += npFolderSize (f); fs.push_back (f); }
            if (total <= cap) return;
            std::sort (fs.begin(), fs.end(), [] (const juce::File& a, const juce::File& b)
                       { return a.getLastModificationTime() < b.getLastModificationTime(); });
            for (auto& f : fs)
            {
                if (total <= cap) break;
                if (used.contains (f.getFileName())) continue;   // protegido: pertenece a un repertorio
                total -= npFolderSize (f);
                f.deleteRecursively();
            }
            sp->refreshStorageStats();
        });
    }

    void setPickerDlPct (const juce::String& id, int pct)
    {
        for (auto& it : repPicker.items) if (it.id == id) { it.dlPct = pct; break; }
        if (repPicker.isVisible()) repPicker.repaint();
    }
    void downloadRepertoireOffline (juce::String id)   // baja TODO un repertorio al caché, sin cambiar la vista
    {
        if (serverToken.isEmpty()) return;
        if (offlineLoader && offlineLoader->isThreadRunning())
        { connStatus.setText (juce::String::fromUTF8 ("Ya hay una descarga en curso\xe2\x80\xa6"), juce::dontSendNotification); return; }
        offlineId = id; offlineTotal = 1; offlinePct = 0;
        for (auto& it : repPicker.items) if (it.id == id) { offlineTotal = juce::jmax (1, it.nCanciones); break; }
        setPickerDlPct (id, 0);
        offlineLoader = std::make_unique<RepertoireLoader> (serverUrl, serverToken, npCacheDir());
        offlineLoader->wantedId = id;
        juce::Component::SafePointer<MainComponent> sp (this);
        offlineLoader->onStatus   = [sp] (juce::String s) { if (sp) sp->connStatus.setText (s, juce::dontSendNotification); };
        offlineLoader->onProgress = [sp] (int i, double f)
        {
            if (! sp) return;
            const int pct = (int) juce::jlimit (0.0, 100.0, ((i + f) / (double) juce::jmax (1, sp->offlineTotal)) * 100.0);
            sp->offlinePct = pct; sp->setPickerDlPct (sp->offlineId, pct);
        };
        offlineLoader->onDone     = [sp] (juce::Array<SongEntry>)
        {
            if (! sp) return;
            for (auto& it : sp->repPicker.items) if (it.id == sp->offlineId) { it.dlPct = -1; it.cached = true; break; }
            if (sp->repPicker.isVisible()) sp->repPicker.repaint();
            sp->connStatus.setText (juce::String::fromUTF8 ("Repertorio descargado \xe2\x9c\x93"), juce::dontSendNotification);
            sp->offlineId.clear(); sp->offlinePct = -1;
            sp->refreshStorageStats();
        };
        offlineLoader->startThread();
        connStatus.setText (juce::String::fromUTF8 ("Descargando repertorio para offline\xe2\x80\xa6"), juce::dontSendNotification);
    }

    void startLoadId (juce::String setlistId)
    {
        if (serverToken.isEmpty()) { connStatus.setText ("Falta servidor/token", juce::dontSendNotification); return; }
        if (loader && loader->isThreadRunning())      // cancelar la carga anterior y reiniciar (no bloquear crear/agregar)
        {
            loader->signalThreadShouldExit();
            loader->stopThread (4000);
        }
        ++loadGen;                                    // invalida callbacks en cola de la carga anterior
        const int gen = loadGen;
        lastSetlistId = setlistId;
        refreshEditAvailability();
        connStatus.setText ("Actualizando...", juce::dontSendNotification);
        loader = std::make_unique<RepertoireLoader> (serverUrl, serverToken, npAppDir().getChildFile ("cache"));
        loader->wantedId = setlistId;
        juce::Component::SafePointer<MainComponent> sp (this);
        loader->onStatus = [sp, gen] (juce::String s) { if (sp && sp->loadGen == gen) { sp->connStatus.setText (s, juce::dontSendNotification); sp->repaint (sp->mapBounds); } };
        loader->onMeta   = [sp, gen] (juce::Array<SongEntry> songs) { if (sp && sp->loadGen == gen) sp->onRepertoireMeta (songs); };
        loader->onProgress = [sp, gen] (int i, double f) { if (sp && sp->loadGen == gen) sp->onSongProgress (i, f); };
        loader->onDone   = [sp, gen] (juce::Array<SongEntry> songs) { if (sp && sp->loadGen == gen) sp->onRepertoireLoaded (songs); };
        loader->startThread();
    }

    void openRepertoirePicker()
    {
        repPicker.loading = true;
        repPicker.items.clearQuick();
        repPicker.selected = -1;
        repPicker.currentLoadedId = lastSetlistId;   // para mostrar "Guardar" en el repertorio cargado
        repPicker.dirty = mixDirty;                  // aviso de cambios sin guardar
        repPicker.setBounds (getLocalBounds());
        repPicker.setVisible (true);
        repPicker.toFront (true);
        repPicker.repaint();

        juce::Component::SafePointer<MainComponent> sp (this);
        const auto url = serverUrl; const auto tok = serverToken;
        juce::Thread::launch ([sp, url, tok]
        {
            auto v = juce::JSON::parse (httpGet (url + "/api/live/setlists", tok));
            juce::Array<RepertoirePicker::Item> its;
            if (auto* sls = v.getProperty ("setlists", juce::var()).getArray())
                for (auto& s : *sls)
                {
                    RepertoirePicker::Item it;
                    it.id     = s.getProperty ("id", "").toString();
                    it.nombre = s.getProperty ("nombre", "Repertorio").toString();
                    it.fecha  = s.getProperty ("fecha", "").toString();
                    bool cached = false;
                    if (auto* cs = s.getProperty ("canciones", juce::var()).getArray())
                    {
                        it.nCanciones = cs->size();
                        cached = ! cs->isEmpty();
                        for (auto& c : *cs)
                        {
                            const int cid = (int) c.getProperty ("id", 0);
                            const int tono = (int) c.getProperty ("tono_semitonos", 0);
                            auto folder = npCacheDir().getChildFile ("song_" + juce::String (cid) + "_t" + juce::String (tono));
                            if (! (folder.isDirectory() && folder.getNumberOfChildFiles (juce::File::findFiles) > 0)) { cached = false; break; }
                        }
                    }
                    it.cached = cached;
                    its.add (it);
                }
            juce::MessageManager::callAsync ([sp, its]
            {
                if (sp != nullptr && sp->repPicker.isVisible())
                {
                    sp->repPicker.setItems (its);
                    if (sp->offlineId.isNotEmpty() && sp->offlineLoader && sp->offlineLoader->isThreadRunning())
                        sp->setPickerDlPct (sp->offlineId, sp->offlinePct);   // re-aplica el % si sigue bajando
                }
            });
        });
    }

    // ¿el audio de esta canción ya está en caché? (tolera .wav/.mp3 y renombres por tono)
    bool cacheReady (const SongEntry& e) const
    {
        if (e.famFiles.isEmpty()) return false;
        for (auto& fn : e.famFiles)
        {
            auto f = e.folder.getChildFile (fn);
            if (f.existsAsFile() && f.getSize() >= 2000) continue;
            // mismo nombre base con otra extensión (p.ej. el tono se rindió en .mp3)
            auto stem = juce::File (fn).getFileNameWithoutExtension();
            bool found = false;
            for (auto& c : e.folder.findChildFiles (juce::File::findFiles, false, stem + ".*"))
                if (c.getSize() >= 2000) { found = true; break; }
            if (! found) return false;
        }
        return true;
    }

    // FASE A: metadata + portadas -> muestra las tarjetas con barra de descarga (aún sin audio)
    void onRepertoireMeta (juce::Array<SongEntry> songs)
    {
        if (loader && loader->resolvedId.isNotEmpty()) lastSetlistId = loader->resolvedId;
        if (loader) currentSetlistName = loader->resolvedName;
        refreshEditAvailability();   // al cargar (incl. al abrir la app) re-habilitar Editar si ya hay repertorio
        repertoire = songs;
        songMaster.clearQuick();
        songMixCache.clearQuick();
        songReady.clearQuick();
        for (int i = 0; i < repertoire.size(); ++i)
        {
            songMaster.add (0.0);
            songMixCache.add (repertoire.getReference (i).mix);
            songReady.add (false);
        }
        for (auto& e : repertoire)
            if (e.coverFile.existsAsFile()) e.cover = juce::ImageFileFormat::loadFrom (e.coverFile);
        currentSong = -1;
        setMixSaved();   // repertorio recién cargado: sin cambios pendientes
        clearSong();
        dlById.clear();   // la barra la maneja SOLO el progreso real de descarga (onSongProgress), nunca un escaneo de disco
        loadOrderIds.clearQuick();
        for (auto& e : repertoire) loadOrderIds.add (e.id);   // orden fijo del loader
        for (int i = 0; i < repertoire.size(); ++i)
            if (i < songReady.size()) songReady.set (i, cacheReady (repertoire.getReference (i)));   // solo para el guard de reproducir
        rebuildRepertoireStrip();
        repaint();
    }

    // FASE B: avance de descarga. 'i' es el índice del LOADER (orden fijo); lo traducimos a id
    // y ubicamos la posición ACTUAL de esa canción (puede haberse reordenado) -> nada de barras falsas.
    void onSongProgress (int i, double f)
    {
        const int id = (i >= 0 && i < loadOrderIds.size()) ? loadOrderIds[i] : -1;
        if (id < 0) return;
        if (f >= 1.0) dlById.erase (id); else dlById[id] = (float) f;

        int idx = -1;                                   // posición actual de la canción (por id)
        for (int j = 0; j < repertoire.size(); ++j)
            if (repertoire.getReference (j).id == id) { idx = j; break; }

        if (idx >= 0 && idx < songCards.size())
        {
            songCards[idx]->dlProgress = (f >= 1.0) ? -1.0f : (float) f;
            songCards[idx]->repaint();
        }
        if (f >= 1.0 && idx >= 0 && idx < songReady.size())
        {
            songReady.set (idx, true);
            if (idx == 0 && currentSong < 0) loadSong (0);   // apenas esté la 1a, cargarla
        }
    }

    // FASE B lista: todo el audio descargado
    void onRepertoireLoaded (juce::Array<SongEntry> songs)
    {
        if (loader && loader->resolvedId.isNotEmpty()) lastSetlistId = loader->resolvedId;
        if (loader) currentSetlistName = loader->resolvedName;   // repertorio vacío llega por acá (sin onMeta)
        refreshEditAvailability();
        repertoire = songs;
        for (auto& e : repertoire)              // la copia del loader trae la ruta pero no la imagen: recargar portadas
            if (! e.cover.isValid() && e.coverFile.existsAsFile())
                e.cover = juce::ImageFileFormat::loadFrom (e.coverFile);
        for (auto* c : songCards) c->dlProgress = -1.0f;
        dlById.clear();                                          // FASE B lista = TODO el audio bajó
        songReady.clearQuick();
        for (int i = 0; i < repertoire.size(); ++i) songReady.add (true);
        if (! didStartupClean) { didStartupClean = true; if (cacheAutoClean) deleteUnusedCache(); enforceCap(); }   // limpieza auto (1 vez, al abrir)
        if (repertoire.isEmpty()) { clearSong(); return; }
        if (currentSong < 0) loadSong (0);
        else repaint (mapBounds);
    }

    void clearSong()   // descarga la canción actual: audio, mapping, secciones, MIDI
    {
        if (mixBuilder) { mixBuilder->stopThread (2000); mixBuilder = nullptr; }
        {
            const juce::ScopedLock sl (graphLock);
            loadingSong.store (true);
            playing.store (false);
            positionOut.store (0);
            seekTo.store (-1);
            resamplers.clear();
            bufferingSources.clear();
            readerSources.clear();
            fileRates.clear();
            trackNames.clear();
            trackServerFam.clear();
            stemFiles.clear();
            curFamFiles.clear();
            curFamNames.clear();
            lengthSamples = 0;
            numTracks = 0;
            loadingSong.store (false);
        }
        sectionTimes.clear();
        sectionNames.clear();
        { const juce::ScopedLock sl (midiLock); currentMidiBoxes.clear(); flushMidiOffs(); }
        thumb.clear();
        currentSong = -1;
        { const juce::ScopedLock l (chartLock); currentChartJson = "{}"; }
        liveSongVer.fetch_add (1);
        playButton.setButtonText ("Play");
        rebuildMixerUI();
        highlightSongButton();
        resized();
        repaint();
    }

    void reorderSong (int fromIdx, int toIdx)
    {
        toIdx = juce::jlimit (0, juce::jmax (0, repertoire.size() - 1), toIdx);
        if (fromIdx < 0 || fromIdx >= repertoire.size()) { rebuildRepertoireStrip(); return; }

        const int curId = (currentSong >= 0 && currentSong < repertoire.size())
                              ? repertoire.getReference (currentSong).id : -1;

        if (toIdx != fromIdx)
        {
            repertoire.move (fromIdx, toIdx);
            if (fromIdx < songMaster.size() && toIdx < songMaster.size()) songMaster.move (fromIdx, toIdx);
            if (fromIdx < songMixCache.size() && toIdx < songMixCache.size()) songMixCache.move (fromIdx, toIdx);
            if (fromIdx < songReady.size() && toIdx < songReady.size()) songReady.move (fromIdx, toIdx);

            if (curId >= 0)
                for (int i = 0; i < repertoire.size(); ++i)
                    if (repertoire.getReference (i).id == curId) { currentSong = i; break; }

            if (! lastSetlistId.isEmpty() && ! serverUrl.isEmpty())   // persistir el orden en el servidor
            {
                juce::StringArray ids;
                for (auto& e : repertoire) ids.add (juce::String (e.id));
                juce::StringPairArray p; p.set ("ids", ids.joinIntoString (","));
                const auto base = serverUrl + "/api/live/setlist/" + lastSetlistId + "/orden";
                const auto tok = serverToken;
                juce::Thread::launch ([base, p, tok] { httpPostForm (base, p, tok); });
            }
        }
        rebuildRepertoireStrip();   // re-acomoda las tarjetas (y hace snap si no cambió)
    }

    void rebuildRepertoireStrip()
    {
        songCards.clear();
        for (int i = 0; i < repertoire.size(); ++i)
        {
            const auto& s = repertoire.getReference (i);
            auto* c = songCards.add (new SongCard());
            c->cover = s.cover;
            c->titulo = s.titulo;
            c->tono = s.tonoNombre;
            c->editMode = editMode;
            c->index = i;
            c->songId = s.id;
            {   // barra ligada al id; si la canción ya está en caché, no hay barra (limpia entradas viejas)
                auto it = dlById.find (s.id);
                if (it != dlById.end() && cacheReady (s)) { dlById.erase (it); it = dlById.end(); }
                c->dlProgress = (it != dlById.end()) ? it->second : -1.0f;
            }
            const int idx = i; const int sid = s.id; const juce::String title = s.titulo;
            c->onClick    = [this, idx, sid] { if (armSongIfMapping (sid)) return; loadSong (idx); };
            c->onRemove   = [this, sid] { removeSong (sid); };
            c->onTono     = [this, sid, title] { openTonoFor (sid, title, false); };
            c->onAddAfter = [this, sid] { openBibliotecaForAddAfter (sid); };
            c->onReorder  = [this] (int from, int to) { reorderSong (from, to); };
            addAndMakeVisible (c);
        }
        addCard.setVisible (editMode && ! lastSetlistId.isEmpty());
        highlightSongButton();
        resized();
    }

    void highlightSongButton()
    {
        for (int i = 0; i < songCards.size(); ++i)
        {
            songCards[i]->active = (i == currentSong);
            songCards[i]->repaint();
        }
    }

    void buildGraph (const juce::File& folder)
    {
        juce::Array<juce::File> files;
        files.addArray (folder.findChildFiles (juce::File::findFiles, false, "*.mp3"));
        files.addArray (folder.findChildFiles (juce::File::findFiles, false, "*.wav"));
        files.sort();
        for (auto& f : files)
        {
            if (f.getFileName().startsWithChar ('.')) continue;
            // Solo stems reales del servidor: ignora (y borra) temporales/huérfanos de descargas cortadas
            if (! curFamFiles.isEmpty() && ! curFamFiles.contains (f.getFileName())) { f.deleteFile(); continue; }
            auto* reader = formatManager.createReaderFor (f);
            if (reader == nullptr) continue;
            lengthSamples = juce::jmax (lengthSamples, (long long) reader->lengthInSamples);
            fileRates.add (reader->sampleRate > 0 ? reader->sampleRate : 44100.0);
            auto* rs = new juce::AudioFormatReaderSource (reader, true);
            rs->setLooping (false);
            readerSources.add (rs);
            bufferingSources.add (new juce::BufferingAudioSource (rs, readThread, false, 88200, 2));
            resamplers.add (new juce::ResamplingAudioSource (bufferingSources.getLast(), false, 2));
            trackNames.add (f.getFileNameWithoutExtension().replaceCharacter ('_', ' '));
            { const int fi = curFamFiles.indexOf (f.getFileName()); trackServerFam.add (fi >= 0 ? curFamNames[fi] : juce::String()); }
            auto nm = f.getFileNameWithoutExtension();
            if (! (nm.containsIgnoreCase ("guide") || nm.containsIgnoreCase ("guia") || nm.containsIgnoreCase ("click")))
                stemFiles.add (f);
        }
        numTracks = resamplers.size();
        computeTrackRouteFam();   // familia canónica por track (para enrutar salidas)
    }

    void loadSong (int index)
    {
        if (index < 0 || index >= repertoire.size()) return;
        if (index < songReady.size() && ! songReady[index]) return;   // audio aún no descargado
        snapshotCurrentMix();   // recuerda la mezcla de la canción anterior
        currentSong = index;
        if (padEnabled.load() && padMode == 0) { padApplyTone(); updatePadUi(); }   // pad en Auto: re-afina a la tonalidad de la canción
        const auto& sng = repertoire.getReference (index);

        if (mixBuilder) { mixBuilder->stopThread (2000); mixBuilder = nullptr; }

        {
            const juce::ScopedLock sl (graphLock);
            loadingSong.store (true);
            playing.store (false);
            positionOut.store (0);
            seekTo.store (-1);
            resamplers.clear();
            bufferingSources.clear();
            readerSources.clear();
            fileRates.clear();
            trackNames.clear();
            trackServerFam.clear();
            stemFiles.clear();
            lengthSamples = 0;
            numTracks = 0;

            curFamFiles = sng.famFiles;
            curFamNames = sng.famNames;
            buildGraph (sng.folder);

            if (currentBlockSize > 0 && ! resamplers.isEmpty())
                for (int i = 0; i < resamplers.size(); ++i)
                {
                    resamplers[i]->setResamplingRatio (fileRates[i] / deviceSampleRate);
                    resamplers[i]->prepareToPlay (currentBlockSize, deviceSampleRate);
                }
            loadingSong.store (false);
        }

        sectionTimes = sng.secTimes;
        sectionNames = sng.secNames;
        bpm = sng.tempo;
        beatsPerBar = sng.beatsPerBar;
        songCompas = sng.compas;
        playButton.setButtonText ("Play");

        thumb.clear();
        if (! stemFiles.isEmpty())
        {
            mixBuilder = std::make_unique<MixThumb> (thumb, formatManager, stemFiles,
                                                     fileRates.isEmpty() ? 44100.0 : fileRates[0], lengthSamples);
            mixBuilder->startThread();
        }

        {
            const double perSong = (index >= 0 && index < songMaster.size()) ? songMaster[index] : 0.0;
            const double mv = masterPerSong ? perSong : globalMasterDb;   // master por canción o general
            masterSlider.setValue (mv, juce::dontSendNotification);
            masterGain.store (dbToGain ((float) mv));
        }
        loopActive.store (false); loopOnce.store (false);
        loopStartSec.store (-1.0); loopEndSec.store (-1.0);
        infiniteBtn.active = false; infiniteBtn.repaint();
        waveImg = juce::Image(); waveDirty = true;

        aplicarInOut (sng.id);                         // #2 punto de inicio/fin de esta canción
        if (songInSec.load() > 0.0) seekSeconds (songInSec.load());

        {
            const juce::ScopedLock sl (midiLock);
            currentMidiBoxes = sng.midiBoxes;
            currentBeatGrid  = sng.beatGrid;
            flushMidiOffs();
            recalcMidiNext (0.0);
            midiCursor = 0.0;
        }
        {   // tempo base de la grilla (mediana de intervalos) para escalar el BPM mostrado
            gridBaseBpm = 0.0;
            juce::Array<double> iv;
            for (int i = 0; i + 1 < currentBeatGrid.size(); ++i)
            {
                const double d = currentBeatGrid[i + 1] - currentBeatGrid[i];
                if (d > 0.02 && d < 4.0) iv.add (d);
            }
            if (iv.size() >= 3) { iv.sort(); const double med = iv[iv.size() / 2]; if (med > 0.0) gridBaseBpm = 60.0 / med; }
        }

        aplicarClickSec (sng.id);                      // #3 sección de click (ya con grilla/tempo listos)

        liveSectionIdx.store (0);
        if (syncEnabled) fetchLiveChart (sng.id, sng.tono);

        rebuildMixerUI();
        if (currentSong >= 0 && currentSong < songMixCache.size() && songMixCache[currentSong].isObject())
            applyMix (songMixCache[currentSong]);   // mezcla del repertorio (si hay)
        else
            applyGlobalOverrides();                 // sin caché: aplicar buses/mute generales si toca
        highlightSongButton();
        resized();
        repaint();
    }

    void rebuildMixerUI()
    {
        for (auto* s : trackSliders) s->setLookAndFeel (nullptr);
        trackSliders.clear();
        trackLabels.clear();
        soloDots.clear();
        for (int i = 0; i < numTracks && i < kMaxTracks; ++i)
        {
            trackGain[i].store (1.0f);
            trackSolo[i].store (false);
            trackLevel[i].store (0.0f);
            auto* s = trackSliders.add (new juce::Slider());
            s->setSliderStyle (juce::Slider::LinearVertical);
            s->setRange (-60.0, 0.0, 0.1);
            s->setValue (0.0);
            s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s->textFromValueFunction = [] (double v) { return dbText (v); };
            s->valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -60.0 : t.getDoubleValue(); };
            const int idx = i;
            s->onValueChange = [this, idx] { trackGain[idx].store (dbToGain ((float) trackSliders[idx]->getValue())); markDirty(); };
            s->setLookAndFeel (&faderLnf);
            s->setRepaintsOnMouseActivity (false);
            faderStrip.addAndMakeVisible (s);

            trackMuted[i].store (false);
            auto* l = trackLabels.add (new ClickLabel());
            l->setText (trackNames[i], juce::dontSendNotification);
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
            l->setFont (juce::Font (13.0f, juce::Font::bold));
            l->onClick = [this, idx]
            {
                if (idx < trackNames.size() && armTrackIfMapping (trackNames[idx])) return;   // #4 armar mute
                toggleTrackMute (idx);
            };
            faderStrip.addAndMakeVisible (l);

            auto* d = soloDots.add (new SoloDot());
            d->onClick = [this, idx]
            {
                if (idx < trackNames.size() && armSoloIfMapping (trackNames[idx])) return;   // #4 armar SOLO
                toggleTrackSolo (idx);
            };
            faderStrip.addAndMakeVisible (d);
        }

        // Orden de faders: Click y Guia primero, luego el resto
        faderOrder.clearQuick();
        juce::Array<int> specials, rest;
        for (int i = 0; i < trackSliders.size(); ++i)
        {
            const auto nm = (i < trackNames.size() ? trackNames[i] : juce::String()).toLowerCase();
            if (nm.contains ("click") || nm.contains ("guia") || nm.contains ("guide") || nm.contains ("cue"))
                specials.add (i);
            else
                rest.add (i);
        }
        numSpecialFaders = specials.size();
        faderOrder.addArray (specials);
        faderOrder.addArray (rest);

        buildBuses();
        updateFaderVisibility();
        if (midiMapMode) setFadersArmable (true);   // #5/#6 mantener faders armables si se rearma la UI en modo MIDI
    }

    void buildBuses()
    {
        for (auto* s : busSliders) s->setLookAndFeel (nullptr);
        busSliders.clear(); busLabels.clear(); busSoloDots.clear();
        familyNames.clear();
        for (int i = 0; i < trackSliders.size() && i < kMaxTracks; ++i)
        {
            juce::String fam = (i < trackServerFam.size() ? trackServerFam[i] : juce::String());
            if (fam.isEmpty()) fam = familyFor (i < trackNames.size() ? trackNames[i] : juce::String());
            int idx = familyNames.indexOf (fam);
            if (idx < 0) { idx = familyNames.size(); familyNames.add (fam); }
            trackFamily[i] = idx;
            trackIsClick[i] = fam.equalsIgnoreCase ("Click");                          // #3 solo click
            trackNoFade[i] = trackIsClick[i]
                          || fam.equalsIgnoreCase (juce::String::fromUTF8 ("Gu\xc3\xad" "a"));   // Click/Guía no suben en el conteo
        }
        for (int f = 0; f < familyNames.size() && f < 16; ++f)
        {
            busGain[f].store (1.0f);
            auto* s = busSliders.add (new juce::Slider());
            s->setSliderStyle (juce::Slider::LinearVertical);
            s->setRange (-60.0, 0.0, 0.1);   // tope = volumen original (sin boost)
            s->setValue (0.0);
            s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s->textFromValueFunction = [] (double v) { return dbText (v); };
            const int bf = f;
            s->onValueChange = [this, bf]
            {
                const double v = busSliders[bf]->getValue();
                busGain[bf].store (dbToGain ((float) v));
                markDirty();
                if (! mixPerSong && ! suppressGlobalSave && bf < familyNames.size()) { globalBusGain[familyNames[bf]] = v; saveStorageCfg(); }
            };
            s->setLookAndFeel (&faderLnf);
            s->setRepaintsOnMouseActivity (false);
            faderStrip.addAndMakeVisible (s);

            auto* l = busLabels.add (new ClickLabel());
            l->setText (familyNames[f], juce::dontSendNotification);
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
            l->setFont (juce::Font (12.0f, juce::Font::bold));
            l->onClick = [this, bf] { if (bf < familyNames.size() && armBusMuteIfMapping (familyNames[bf])) return; toggleBusMute (bf); };
            faderStrip.addAndMakeVisible (l);

            auto* d = busSoloDots.add (new SoloDot());
            d->onClick = [this, bf] { if (bf < familyNames.size() && armBusSoloIfMapping (familyNames[bf])) return; toggleBusSolo (bf); };
            faderStrip.addAndMakeVisible (d);
        }
        refreshBusStates();
    }

    void toggleBusMute (int f)
    {
        bool allM = true; int cnt = 0;
        for (int i = 0; i < trackSliders.size(); ++i)
            if (trackFamily[i] == f) { ++cnt; if (! trackMuted[i].load()) allM = false; }
        const bool nu = ! (cnt > 0 && allM);
        for (int i = 0; i < trackSliders.size(); ++i)
            if (trackFamily[i] == f)
            {
                trackMuted[i].store (nu);
                trackSliders[i]->getProperties().set ("muted", nu);
                trackSliders[i]->repaint();
                trackLabels[i]->setColour (juce::Label::textColourId, nu ? juce::Colour (0xffe05555) : juce::Colour (0xfff2f2f2));
                trackLabels[i]->repaint();
            }
        if (! mixPerSong && ! suppressGlobalSave && f < familyNames.size())   // mezcla general: mute por FAMILIA (bus)
        {
            if (nu) globalMutedFamilies.insert (familyNames[f]);
            else                                                              // al desmutear la familia, se limpian sus canales sueltos
            {
                globalMutedFamilies.erase (familyNames[f]);
                for (int i = 0; i < trackNames.size(); ++i) if (trackFamily[i] == f) globalMuted.erase (trackNames[i]);
            }
            saveStorageCfg();
        }
        markDirty();
        refreshBusStates();
    }

    void toggleBusSolo (int f)
    {
        bool allS = true; int cnt = 0;
        for (int i = 0; i < trackSliders.size(); ++i)
            if (trackFamily[i] == f) { ++cnt; if (! trackSolo[i].load()) allS = false; }
        const bool nu = ! (cnt > 0 && allS);
        for (int i = 0; i < trackSliders.size(); ++i)
            if (trackFamily[i] == f)
            {
                trackSolo[i].store (nu);
                soloDots[i]->on = nu; soloDots[i]->repaint();
            }
        markDirty();
        refreshBusStates();
    }

    void refreshBusStates()
    {
        for (int f = 0; f < busSliders.size(); ++f)
        {
            bool allM = true, allS = true; int cnt = 0;
            for (int i = 0; i < trackSliders.size(); ++i)
                if (trackFamily[i] == f) { ++cnt; if (! trackMuted[i].load()) allM = false; if (! trackSolo[i].load()) allS = false; }
            const bool muted  = (cnt > 0 && allM);
            const bool soloed = (cnt > 0 && allS);
            busLabels[f]->setColour (juce::Label::textColourId, muted ? juce::Colour (0xffe05555) : juce::Colour (0xfff2f2f2));
            busLabels[f]->repaint();
            busSliders[f]->getProperties().set ("muted", muted);
            busSliders[f]->repaint();
            busSoloDots[f]->on = soloed; busSoloDots[f]->repaint();
        }
    }

    void updateFaderVisibility()
    {
        const bool tracks = (faderView == 0);
        const bool buses  = (faderView == 1);
        const bool midi   = (faderView == 2);
        const bool pad    = (faderView == 3);
        faderViewport.setVisible (! midi && ! pad);
        midiPanel.setVisible (midi);
        padPanel.setVisible (pad);
        for (auto* s : trackSliders) s->setVisible (tracks);
        for (auto* l : trackLabels)  l->setVisible (tracks);
        for (auto* d : soloDots)     d->setVisible (tracks);
        for (auto* s : busSliders)   s->setVisible (buses);
        for (auto* l : busLabels)    l->setVisible (buses);
        for (auto* d : busSoloDots)  d->setVisible (buses);
    }

    void setFaderView (int v)
    {
        faderView = juce::jlimit (0, 3, v);
        faderViewBtn.active   = (faderView == 0);
        busesBtn.setColour     (juce::TextButton::buttonColourId, faderView == 1 ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff1f1f1f));
        padPlayerBtn.setColour (juce::TextButton::buttonColourId, faderView == 3 ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff1f1f1f));
        muteMidiBtn.setColour  (juce::TextButton::buttonColourId, faderView == 2 ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff1f1f1f));
        faderViewBtn.repaint(); busesBtn.repaint(); padPlayerBtn.repaint(); muteMidiBtn.repaint();
        if (faderView == 1) refreshBusStates();
        if (faderView == 2) midiPanel.refreshPorts();
        if (faderView == 3) refreshPadPanel();
        updateFaderVisibility();
        if (faderView == 0 || faderView == 1) layoutFaderStrip();
    }

    void rebuildMidiOuts()
    {
        const juce::ScopedLock sl (midiLock);
        flushMidiOffs();
        midiOuts.clear();
        cajaOut.clearQuick();
        auto devs = juce::MidiOutput::getAvailableDevices();
        juce::StringArray openedIds;
        for (int i = 0; i < midiPanel.count(); ++i)
        {
            juce::MidiOutput* out = nullptr;
            const auto pn = midiPanel.portName (i);
            if (pn.isNotEmpty())
            {
                juce::String ident;
                for (auto& d : devs) if (d.name == pn) { ident = d.identifier; break; }
                if (ident.isNotEmpty())
                {
                    const int idx = openedIds.indexOf (ident);
                    if (idx < 0)
                    {
                        if (auto mo = juce::MidiOutput::openDevice (ident))
                        {
                            openedIds.add (ident);
                            out = mo.get();
                            midiOuts.add (mo.release());
                        }
                    }
                    else out = midiOuts[idx];
                }
            }
            cajaOut.add (out);
        }
        for (int i = 0; i < 8; ++i)
        {
            cajaOnArr[i]   = midiPanel.isOn (i);
            cajaChanArr[i] = midiPanel.channel (i);
        }
        // salida del MIDI Clock (sin canal)
        midiClockOutPtr  = nullptr;
        clockEnabledFlag = midiPanel.clockOn();
        {
            const auto pn = midiPanel.clockPortName();
            if (pn.isNotEmpty())
            {
                juce::String ident;
                for (auto& d : devs) if (d.name == pn) { ident = d.identifier; break; }
                if (ident.isNotEmpty())
                {
                    const int idx = openedIds.indexOf (ident);
                    if (idx >= 0) midiClockOutPtr = midiOuts[idx];
                    else if (auto mo = juce::MidiOutput::openDevice (ident))
                    { openedIds.add (ident); midiClockOutPtr = mo.get(); midiOuts.add (mo.release()); }
                }
            }
        }
    }
    void flushMidiOffs()
    {
        for (auto& o : midiOffs) if (o.out != nullptr) o.out->sendMessageNow (juce::MidiMessage::noteOff (o.chan, o.note));
        midiOffs.clearQuick();
    }
    void recalcMidiNext (double pos)
    {
        for (int ci = 0; ci < 8; ++ci)
        {
            int idx = 0;
            if (ci < currentMidiBoxes.size())
            {
                const auto& notas = currentMidiBoxes[ci].notas;
                while (idx < notas.size() && notas[idx].seg < pos) ++idx;
            }
            midiNext[ci] = idx;
        }
    }
    double currentBpm() const   // BPM local según la posición (sigue cambios de tempo en medleys), estable
    {
        const auto& g = currentBeatGrid;
        const int n = g.size();
        if (n >= 6 && bpm > 0.0 && gridBaseBpm > 0.0)
        {
            const double t = positionSeconds();
            int i = 0;
            while (i + 1 < n && g[i + 1] <= t) ++i;
            // promedio sobre una ventana de ~8 negras (mata el jitter de la detección del click)
            const int a = juce::jlimit (0, n - 1, i - 4);
            const int b = juce::jlimit (0, n - 1, i + 4);
            if (b - a >= 2 && g[b] > g[a])
            {
                const double localBpm = 60.0 * (b - a) / (g[b] - g[a]);
                // la grilla puede venir a x2/x4 (corcheas/semicorcheas); dividimos por el factor real
                // deducido del tempo declarado, así 250->125 al arrancar y 280->140 al subir
                const int div = juce::jlimit (1, 4, (int) std::llround (gridBaseBpm / bpm));
                return localBpm / (double) div;
            }
        }
        return bpm;
    }
    void seekBar (int dir)   // saltar al compás anterior/siguiente
    {
        const auto& g = currentBeatGrid;
        const int nb0 = juce::jmax (1, beatsPerBar);
        const int div = (gridBaseBpm > 0.0 && bpm > 0.0) ? juce::jlimit (1, 4, (int) std::llround (gridBaseBpm / bpm)) : 1;
        const int nb  = nb0 * div;   // puntos de grilla por compás real (la grilla puede venir a x2/x4)
        if (g.size() >= 2)
        {
            const double t = positionSeconds();
            int i = 0; while (i + 1 < g.size() && g[i + 1] <= t + 0.03) ++i;
            int target = ((i / nb) + dir) * nb;
            target = juce::jlimit (0, g.size() - 1, target);
            seekSeconds (g[target]);
        }
        else if (bpm > 0.0)
        {
            const double secPerBar = 60.0 / bpm * nb0;
            seekSeconds (juce::jmax (0.0, positionSeconds() + dir * secPerBar));
        }
    }
    void seekSection (int dir)   // saltar al inicio de la sección anterior/siguiente
    {
        if (sectionTimes.isEmpty()) { seekBar (dir); return; }   // respaldo: por compás
        juce::Array<double> anchors;
        if (sectionTimes[0] > 0.4) anchors.add (0.0);            // bloque "Conteo"
        for (auto s : sectionTimes) anchors.add (s);
        const double t = positionSeconds();
        const double eps = 0.25;
        double target;
        if (dir > 0)
        {
            target = anchors[anchors.size() - 1];
            for (auto a : anchors) if (a > t + eps) { target = a; break; }
        }
        else
        {
            target = 0.0;
            for (int i = anchors.size() - 1; i >= 0; --i) if (anchors[i] < t - eps) { target = anchors[i]; break; }
        }
        seekSeconds (juce::jlimit (0.0, totalSeconds(), target));
    }
    double pulsesAt (double t) const   // pulsos de MIDI clock (24 PPQN) acumulados hasta t
    {
        const auto& g = currentBeatGrid;
        const int n = g.size();
        if (n >= 2)   // grilla de negras: sigue cualquier cambio de tempo (medleys/rubato)
        {
            if (t <= g[0]) { const double dt = g[1] - g[0]; return dt > 0 ? 24.0 * (t - g[0]) / dt : 0.0; }
            for (int i = 0; i + 1 < n; ++i)
                if (t < g[i + 1]) { const double dt = g[i + 1] - g[i]; return dt > 0 ? i * 24.0 + 24.0 * (t - g[i]) / dt : i * 24.0; }
            const double dt = g[n - 1] - g[n - 2];
            return dt > 0 ? (n - 1) * 24.0 + 24.0 * (t - g[n - 1]) / dt : (n - 1) * 24.0;
        }
        return t * (bpm / 60.0) * 24.0;   // fallback: tempo único
    }

    void procesarClock (bool pl, double cur)   // MIDI Clock (24 PPQN) enganchado al audio; sin canal
    {
        if (midiClockOutPtr == nullptr || ! clockEnabledFlag || (bpm <= 0.0 && currentBeatGrid.size() < 2))
        {
            if (clockRunning && midiClockOutPtr != nullptr) midiClockOutPtr->sendMessageNow (juce::MidiMessage::midiStop());
            clockRunning = false;
            return;
        }
        if (! pl)
        {
            if (clockRunning) { midiClockOutPtr->sendMessageNow (juce::MidiMessage::midiStop()); clockRunning = false; }
            return;
        }
        const long long target = (long long) std::floor (pulsesAt (cur));
        if (! clockRunning)
        {
            const int spp = (int) std::floor (pulsesAt (cur) / 6.0);   // posición en semicorcheas (6 pulsos)
            if (cur > 0.05) { midiClockOutPtr->sendMessageNow (juce::MidiMessage::songPositionPointer (spp)); midiClockOutPtr->sendMessageNow (juce::MidiMessage::midiContinue()); }
            else            { midiClockOutPtr->sendMessageNow (juce::MidiMessage::midiStart()); }
            clockPulses = target; clockRunning = true;
            return;
        }
        long long delta = target - clockPulses;
        if (delta < 0 || delta > 48)   // seek: resincronizar sin inundar de pulsos
        {
            const int spp = (int) std::floor (pulsesAt (cur) / 6.0);
            midiClockOutPtr->sendMessageNow (juce::MidiMessage::songPositionPointer (spp));
            midiClockOutPtr->sendMessageNow (juce::MidiMessage::midiContinue());
            clockPulses = target;
            return;
        }
        for (long long k = 0; k < delta; ++k) midiClockOutPtr->sendMessageNow (juce::MidiMessage::midiClock());
        clockPulses = target;
    }

    void fireMidiRT()   // hilo dedicado de alta resolucion (~1ms)
    {
        const juce::ScopedLock sl (midiLock);
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        for (int i = midiOffs.size(); --i >= 0;)
            if (midiOffs[i].t <= now)
            {
                if (midiOffs[i].out != nullptr) midiOffs[i].out->sendMessageNow (juce::MidiMessage::noteOff (midiOffs[i].chan, midiOffs[i].note));
                midiOffs.remove (i);
            }
        const bool pl = playing.load();
        const double cur = (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate);
        procesarClock (pl, cur);
        if (! pl)
        {
            if (! midiOffs.isEmpty()) flushMidiOffs();
            recalcMidiNext (cur);
            midiCursor = cur;
            return;
        }
        if (cur < midiCursor - 0.06 || cur > midiCursor + 1.0)   // seek
        {
            flushMidiOffs();
            recalcMidiNext (cur);
            midiCursor = cur;
            return;
        }
        for (int ci = 0; ci < currentMidiBoxes.size() && ci < 8; ++ci)
        {
            auto* out = (ci < cajaOut.size() ? cajaOut[ci] : nullptr);
            if (out == nullptr || ! cajaOnArr[ci]) continue;
            const int chan = cajaChanArr[ci];
            const auto& notas = currentMidiBoxes[ci].notas;
            while (midiNext[ci] < notas.size() && notas[midiNext[ci]].seg <= cur)
            {
                const auto& n = notas[midiNext[ci]];
                out->sendMessageNow (juce::MidiMessage::noteOn (chan, n.note, (juce::uint8) juce::jlimit (1, 127, n.vel)));
                midiOffs.add ({ out, chan, n.note, now + 140 });
                ++midiNext[ci];
            }
        }
        midiCursor = cur;
    }

    void currentSectionRange (double pos, double& t0, double& t1) const
    {
        const double total = totalSeconds();
        t0 = 0.0; t1 = total;
        if (sectionTimes.isEmpty()) return;
        if (pos < sectionTimes[0]) { t0 = 0.0; t1 = sectionTimes[0]; return; }   // Conteo
        for (int i = 0; i < sectionTimes.size(); ++i)
        {
            const double a = sectionTimes[i];
            const double b = (i + 1 < sectionTimes.size() ? sectionTimes[i + 1] : total);
            if (pos >= a && pos < b) { t0 = a; t1 = b; return; }
        }
    }

    void getViewWindow (double& vs, double& ve) const
    {
        const double total = totalSeconds();
        const double win = juce::jmin (20.0, juce::jmax (4.0, total));
        const double center = browsing ? browseCenter : positionSeconds();
        // Aguja fija al inicio (borde izquierdo): el mapa se desplaza por debajo
        vs = center;
        ve = vs + win;
    }
    double totalSeconds() const
    {
        const double fr = fileRates.isEmpty() ? 44100.0 : fileRates[0];
        return lengthSamples > 0 ? (double) lengthSamples / fr : 1.0;
    }
    double positionSeconds() const
    {
        const double t = clickSecArmed.load() ? (totalSeconds() + clickLenSec.load()) : totalSeconds();   // #3 permite el bloque de click
        return t > 0 ? juce::jlimit (0.0, t, (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate)) : 0.0;
    }
    void timerCallback() override
    {
        if (splashOn && juce::Time::getMillisecondCounter() - splashStart > 1600) { splashOn = false; splash.setVisible (false); }
        updatePadAutomation();   // Pad Player: intro/outro por canción
        reapDeadPadVoices();     // libera voces de pad marcadas en mixPad (fuera del hilo de audio)

        if (syncEnabled)   // puente: sección en vivo + heartbeat 30s + estado 5s
        {
            liveSectionIdx.store (liveSectionIndexAt (positionSeconds()));
            if (++syncPingCtr >= 1800) { syncPingCtr = 0; syncPing (false); }
            if (++syncPollCtr >= 300)  { syncPollCtr = 0; syncPoll(); }
        }
        {
            const bool nowP = playing.load();
            if (nowP != lastPlaying)   // cambio play<->stop (incluye fin de cancion)
            {
                playButton.setButtonText (nowP ? "Pausa" : "Play");
                lastPlaying = nowP;
                repaint (mapBounds);
            }
        }
        timeLabel.setText (fmtTime (positionSeconds()) + "\n" + fmtTime (totalSeconds()), juce::dontSendNotification);
        if (browsing && ! isDragging && (juce::Time::getMillisecondCounter() - lastInteractionMs > 1200))
        { browsing = false; repaint (mapBounds); }
        if (playing.load() || browsing) repaint (mapBounds);
        if (playing.load() && currentBeatGrid.size() >= 2) repaint (compasBoxBounds);   // BPM que sigue el medley

        if (fadeDir != 0)
        {
            bool done = true;
            const double step = 0.33;  // dB por tick (~3s de desvanecimiento)
            const bool prevSup = suppressGlobalSave; suppressGlobalSave = true;   // el fade no marca "sin guardar"
            for (int i = 0; i < trackSliders.size(); ++i)
            {
                auto* s = trackSliders[i];
                const double target = (fadeDir < 0) ? s->getMinimum()
                                                    : (i < preFadeVals.size() ? preFadeVals[i] : 0.0);
                double v = s->getValue();
                if (std::abs (target - v) <= step) v = target;
                else                                v += (target > v ? step : -step);
                if (v != s->getValue()) s->setValue (v, juce::sendNotificationSync);
                if (v != target) done = false;
            }
            suppressGlobalSave = prevSup;
            if (done) fadeDir = 0;
        }

        if (((++vuTick) & 1) == 0)   // VU a ~30Hz para no saturar el hilo grafico (mapa fluido)
        {
            const bool pl = playing.load();
            bool anyLvl = false;
            for (int i = 0; i < trackSliders.size(); ++i)
            {
                float lv = trackLevel[i].load();
                if (! pl) { lv *= 0.55f; if (lv < 0.001f) lv = 0.0f; trackLevel[i].store (lv); }
                if (lv > 0.02f) anyLvl = true;
                trackSliders[i]->getProperties().set ("lvl", (double) lv);
            }
            if (anyLvl || pl) for (auto* s : trackSliders) s->repaint();
        }

        if (repeatBtn.active != loopOnce.load()) { repeatBtn.active = loopOnce.load(); repeatBtn.repaint(); }
        // #3 el ∞ solo se ve encendido cuando el playhead está EN el bloque de click (no confunde en otras partes)
        const bool inClk = clickSecArmed.load() && clickLoopOn.load() && positionSeconds() >= totalSeconds() - 0.05;
        const bool infOn = loopActive.load() || inClk;
        if (infiniteBtn.active != infOn) { infiniteBtn.active = infOn; infiniteBtn.repaint(); }
    }
    void togglePlay()
    {
        if (editMode || resamplers.isEmpty()) return;   // en edición no se reproduce
        const bool p = ! playing.load();
        // al final: reiniciar — PERO no si estás en el bloque de click (ahí se reanuda libre)
        if (p && positionSeconds() >= totalSeconds() - 0.1
            && ! (clickSecArmed.load() && positionSeconds() >= totalSeconds() - 0.05))
            seekSeconds (0.0);
        if (p && countInEnabled) prepararConteo();       // #1 conteo con entrada de faders (si está al inicio de una sección)
        else                     countInActive.store (false);
        playing.store (p);
        playButton.setButtonText (p ? "Pausa" : "Play");
    }

    double tiempoUnCompasAntes (double ts) const   // 1 compás antes de ts (por grilla o bpm)
    {
        const auto& g = currentBeatGrid;
        const int nb0 = juce::jmax (1, beatsPerBar);
        const int div = (gridBaseBpm > 0.0 && bpm > 0.0) ? juce::jlimit (1, 4, (int) std::llround (gridBaseBpm / bpm)) : 1;
        const int nb  = nb0 * div;
        if (g.size() >= 2)
        {
            int i = 0; while (i + 1 < g.size() && g[i + 1] <= ts + 0.03) ++i;
            return g[juce::jlimit (0, g.size() - 1, i - nb)];
        }
        if (bpm > 0.0) return juce::jmax (0.0, ts - 60.0 / bpm * nb0);
        return ts;
    }

    void prepararConteo()   // si el playhead está justo al inicio de una sección, arma el conteo con swell
    {
        countInActive.store (false);
        const double pos = positionSeconds();
        double ts = -1.0;
        for (auto& s : sectionTimes) if (s > 0.05 && std::abs (s - pos) < 0.18) { ts = s; break; }
        if (ts < 0.0) return;                          // no está en un inicio de sección -> play normal
        const double ci = tiempoUnCompasAntes (ts);
        if (ci >= ts - 0.05) return;                   // sin espacio para el conteo
        seekSeconds (ci);
        countInStartSec.store (ci);
        countInEndSec.store (ts);
        countInActive.store (true);
    }

    // #3 sección de click al final (persistente por canción)
    void loadClickSec()
    {
        clickSecSongs.clear();
        auto v = juce::JSON::parse (npAppDir().getChildFile ("clicksec.json"));
        if (auto* a = v.getArray()) for (auto& e : *a) { const int id = (int) e; if (id > 0) clickSecSongs.insert (id); }
    }
    void saveClickSec()
    {
        juce::Array<juce::var> a;
        for (int id : clickSecSongs) a.add (id);
        npAppDir().getChildFile ("clicksec.json").replaceWithText (juce::JSON::toString (juce::var (a)));
    }
    double clickSecLen() const { return clickLenSec.load(); }               // largo del bloque de click
    double clickBlockEnd() const { return totalSeconds() + clickLenSec.load(); }
    bool   inClickBlock (double s) const
    { return songHasClickSec && clickLenSec.load() > 0.0 && s >= totalSeconds() - 1.0e-4 && s <= clickBlockEnd() + 1.0e-4; }

    void aplicarClickSec (int songId)   // fija estado de la sección de click para la canción actual
    {
        songHasClickSec = clickSecSongs.count (songId) > 0;
        clickSecArmed.store (songHasClickSec);
        if (songHasClickSec && totalSeconds() > 0.0 && bpm > 0.0)
        {
            const int nb = juce::jmax (1, beatsPerBar) * 2;                 // 2 compases
            clickLenSec.store (60.0 / bpm * nb);
            clickLoopOn.store (true);                                        // ∞ ON por defecto
        }
        else { clickLenSec.store (0.0); clickLoopOn.store (false); }
        clkLastBeat = -1;
    }
    void agregarSeccionClick()   // + : agrega la sección de click (persiste, ∞ ON por defecto)
    {
        if (currentSong < 0 || currentSong >= repertoire.size()) return;
        const int id = repertoire.getReference (currentSong).id;
        clickSecSongs.insert (id);
        saveClickSec();
        aplicarClickSec (id);
        infiniteBtn.active = clickLoopOn.load(); infiniteBtn.repaint();
        repaint (mapBounds);
    }
    void quitarSeccionClick()   // − : elimina la sección de click de esta canción
    {
        if (currentSong < 0 || currentSong >= repertoire.size()) return;
        const int id = repertoire.getReference (currentSong).id;
        clickSecSongs.erase (id);
        saveClickSec();
        clickLoopOn.store (false);
        if (positionSeconds() > totalSeconds()) seekSeconds (juce::jmax (0.0, totalSeconds() - 0.05));
        aplicarClickSec (id);
        infiniteBtn.active = false; infiniteBtn.repaint();
        repaint (mapBounds);
    }

    void toggleRepeatOnce()   // repetir una vez la sección actual
    {
        if (loopOnce.load())
        {
            loopOnce.store (false);
            if (! loopActive.load()) { loopStartSec.store (-1.0); loopEndSec.store (-1.0); }
        }
        else
        {
            double t0, t1; currentSectionRange (positionSeconds(), t0, t1);
            loopStartSec.store (t0); loopEndSec.store (t1); loopOnce.store (true);
        }
        repeatBtn.active = loopOnce.load(); repeatBtn.repaint();   // instantáneo, sin esperar el timer
        repaint (mapBounds);
    }
    void toggleLoopInfinite()   // loop infinito de la sección actual (o de la sección de click si estamos en ella)
    {
        if (songHasClickSec && clickLenSec.load() > 0.0 && positionSeconds() >= totalSeconds() - 0.15)
        {   // #3 en/cerca del bloque de click, el ∞ enciende/apaga su loop (reactivable, sin saltos)
            const bool on = ! clickLoopOn.load();
            clickLoopOn.store (on);
            if (on && positionSeconds() < totalSeconds()) seekSeconds (totalSeconds());   // entrar al bloque
            infiniteBtn.active = on; infiniteBtn.repaint();
            repaint (mapBounds);
            return;
        }
        const bool on = ! loopActive.load();
        if (on)
        {
            double t0, t1; currentSectionRange (positionSeconds(), t0, t1);
            loopStartSec.store (t0); loopEndSec.store (t1); loopActive.store (true);
        }
        else
        {
            loopActive.store (false); loopOnce.store (false);
            loopStartSec.store (-1.0); loopEndSec.store (-1.0);
        }
        infiniteBtn.active = on; infiniteBtn.repaint();
        repaint (mapBounds);
    }

    // ── #4 mapping de teclado ──
    void doAct (int a)
    {
        switch (a)
        {
            case kaPlay:      togglePlay();        break;
            case kaReturn:    seekSeconds (0.0);   break;
            case kaPrevBar:   seekSection (-1);    break;
            case kaNextBar:   seekSection (+1);    break;
            case kaFade:      toggleFade();        break;
            case kaLoop:      toggleLoopInfinite(); break;
            case kaRepeat:    toggleRepeatOnce();  break;
            case kaPad:       padBtn.triggerClick(); break;
            case kaBuses:     setFaderView (1);    break;
            case kaMidi:      setFaderView (2);    break;
            case kaFaderView: setFaderView (0);    break;
            case kaPadPlayer: padPlayerBtn.triggerClick(); break;
            default: break;
        }
    }
    bool mapping() const { return keyMapMode || midiMapMode; }   // algún modo de asignación activo
    bool clickOrArm (int a)   // en modo mapping arma la acción fija y NO la ejecuta; true si armó
    {
        if (! mapping()) return false;
        clearArm(); armKind = 1; armedAct = a; repaint(); return true;
    }
    bool armTrackIfMapping (const juce::String& name)   // MUTE
    {
        if (! mapping()) return false;
        clearArm(); armKind = 2; armTrack = name; repaint(); return true;
    }
    bool armSoloIfMapping (const juce::String& name)     // SOLO
    {
        if (! mapping()) return false;
        clearArm(); armKind = 4; armTrack = name; repaint(); return true;
    }
    bool armBusMuteIfMapping (const juce::String& name)
    {
        if (! mapping()) return false;
        clearArm(); armKind = 5; armTrack = name; repaint(); return true;
    }
    bool armBusSoloIfMapping (const juce::String& name)
    {
        if (! mapping()) return false;
        clearArm(); armKind = 6; armTrack = name; repaint(); return true;
    }
    bool armSongIfMapping (int songId)
    {
        if (! mapping()) return false;
        clearArm(); armKind = 3; armSong = songId; repaint(); return true;
    }
    bool armFaderIfMidi (int trackIdx, const juce::String& busName, bool master)   // solo en modo MIDI
    {
        if (! midiMapMode) return false;
        clearArm(); armFader = true;
        armFaderIdx = master ? -1 : (busName.isNotEmpty() ? -2 : trackIdx);
        armTrack = busName;
        repaint(); return true;
    }
    void clearAllForKey (int code)   // quita esa tecla de cualquier asignación previa (sin duplicados)
    {
        for (int a = 0; a < kaCount; ++a) if (actKey[a] == code) actKey[a] = 0;
        for (auto& kv : keyByTrack)   if (kv.second == code) kv.second = 0;
        for (auto& kv : keyBySolo)    if (kv.second == code) kv.second = 0;
        for (auto& kv : keyByBusMute) if (kv.second == code) kv.second = 0;
        for (auto& kv : keyByBusSolo) if (kv.second == code) kv.second = 0;
        for (auto& kv : keyBySong)    if (kv.second == code) kv.second = 0;
    }
    static juce::String keyLabel (int code)
    {
        if (code <= 0) return {};
        if (code == juce::KeyPress::spaceKey)  return juce::String::fromUTF8 ("Espacio");
        if (code == juce::KeyPress::leftKey)   return juce::String::fromUTF8 ("\xe2\x86\x90");
        if (code == juce::KeyPress::rightKey)  return juce::String::fromUTF8 ("\xe2\x86\x92");
        if (code == juce::KeyPress::upKey)     return juce::String::fromUTF8 ("\xe2\x86\x91");
        if (code == juce::KeyPress::downKey)   return juce::String::fromUTF8 ("\xe2\x86\x93");
        if (code == juce::KeyPress::returnKey) return juce::String::fromUTF8 ("\xe2\x8f\x8e");
        if (code >= 33 && code < 127) return juce::String::charToString ((juce_wchar) code).toUpperCase();
        return "?";
    }
    void loadKeyMap()
    {
        for (auto& k : actKey) k = 0;
        keyByTrack.clear(); keyBySolo.clear(); keyByBusMute.clear(); keyByBusSolo.clear(); keyBySong.clear();
        auto v = juce::JSON::parse (npAppDir().getChildFile ("keymap.json"));
        if (auto* o = v.getDynamicObject())
        {
            for (int a = 0; a < kaCount; ++a) actKey[a] = (int) o->getProperty (juce::String (a));
            if (auto* tk = o->getProperty ("tracks").getDynamicObject())
                for (auto& pr : tk->getProperties()) keyByTrack[pr.name.toString()] = (int) pr.value;
            if (auto* so = o->getProperty ("solos").getDynamicObject())
                for (auto& pr : so->getProperties()) keyBySolo[pr.name.toString()] = (int) pr.value;
            if (auto* bm = o->getProperty ("busmutes").getDynamicObject())
                for (auto& pr : bm->getProperties()) keyByBusMute[pr.name.toString()] = (int) pr.value;
            if (auto* bs = o->getProperty ("bussolos").getDynamicObject())
                for (auto& pr : bs->getProperties()) keyByBusSolo[pr.name.toString()] = (int) pr.value;
            if (auto* sg = o->getProperty ("songs").getDynamicObject())
                for (auto& pr : sg->getProperties()) keyBySong[pr.name.toString().getIntValue()] = (int) pr.value;
        }
    }
    void saveKeyMap()
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        for (int a = 0; a < kaCount; ++a) o->setProperty (juce::String (a), actKey[a]);
        juce::DynamicObject::Ptr tk = new juce::DynamicObject();
        for (auto& kv : keyByTrack) if (kv.second != 0) tk->setProperty (kv.first, kv.second);
        juce::DynamicObject::Ptr so = new juce::DynamicObject();
        for (auto& kv : keyBySolo) if (kv.second != 0) so->setProperty (kv.first, kv.second);
        juce::DynamicObject::Ptr bm = new juce::DynamicObject();
        for (auto& kv : keyByBusMute) if (kv.second != 0) bm->setProperty (kv.first, kv.second);
        juce::DynamicObject::Ptr bs = new juce::DynamicObject();
        for (auto& kv : keyByBusSolo) if (kv.second != 0) bs->setProperty (kv.first, kv.second);
        juce::DynamicObject::Ptr sg = new juce::DynamicObject();
        for (auto& kv : keyBySong) if (kv.second != 0) sg->setProperty (juce::String (kv.first), kv.second);
        o->setProperty ("tracks",   juce::var (tk.get()));
        o->setProperty ("solos",    juce::var (so.get()));
        o->setProperty ("busmutes", juce::var (bm.get()));
        o->setProperty ("bussolos", juce::var (bs.get()));
        o->setProperty ("songs",    juce::var (sg.get()));
        npAppDir().getChildFile ("keymap.json").replaceWithText (juce::JSON::toString (juce::var (o.get())));
    }
    void clearArm() { armKind = 0; armedAct = -1; armSong = -1; armTrack = {}; armFader = false; armFaderIdx = -1; }
    void toggleKeyMapMode()
    {
        keyMapMode = ! keyMapMode;
        clearArm();
        if (keyMapMode) { midiMapMode = false; setFadersArmable (false); grabKeyboardFocus(); }
        updateTransportEnabled();
        resized();
        repaint();
    }

    // ─────────── #5/#6 MIDI IN + learn ───────────
    void openMidiInputs()   // abre TODAS las entradas MIDI disponibles y escucha
    {
        midiInputs.clear();
        for (auto& d : juce::MidiInput::getAvailableDevices())
        {
            if (auto in = juce::MidiInput::openDevice (d.identifier, this))
            { in->start(); midiInputs.add (in.release()); }
        }
    }
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {   // llega en el hilo MIDI -> pasar al hilo de mensajes
        juce::Component::SafePointer<MainComponent> sp (this);
        const juce::MidiMessage msg (m);
        juce::MessageManager::callAsync ([sp, msg] { if (sp) sp->onMidiMessage (msg); });
    }
    static int midiTrigCode (const juce::MidiMessage& m)   // código de disparador (Note o CC-botón); 0 si no aplica
    {
        const int ch = juce::jlimit (1, 16, m.getChannel()) - 1;
        if (m.isNoteOn())      return 1000000 + ch * 128 + m.getNoteNumber();
        if (m.isController())  return 2000000 + ch * 128 + m.getControllerNumber();
        return 0;
    }
    static int midiCcCode (const juce::MidiMessage& m)     // código de un CC (para faders continuos); 0 si no es CC
    { return m.isController() ? (2000000 + (juce::jlimit (1,16,m.getChannel())-1) * 128 + m.getControllerNumber()) : 0; }
    static juce::String midiLabel (int code)
    {
        if (code <= 0) return {};
        const int type = code / 1000000, n = code % 128;
        return (type == 1 ? "N" : "CC") + juce::String (n);
    }
    void clearAllForMidi (int code)   // quita ese código MIDI de cualquier asignación previa (sin duplicados)
    {
        for (auto& v : actMidi) if (v == code) v = 0;
        for (auto* mp : { &midiTrackMute, &midiTrackSolo, &midiBusMute, &midiBusSolo, &midiTrackFader, &midiBusFader })
            for (auto& kv : *mp) if (kv.second == code) kv.second = 0;
        for (auto& kv : midiSong) if (kv.second == code) kv.second = 0;
        if (midiMasterFader == code) midiMasterFader = 0;
        if (midiPadFader == code) midiPadFader = 0;
    }
    void setFadersArmable (bool on)   // en modo MIDI, los faders no se arrastran: el click los arma
    {
        for (auto* s : trackSliders) s->setInterceptsMouseClicks (! on, ! on);
        for (auto* s : busSliders)   s->setInterceptsMouseClicks (! on, ! on);
        masterSlider.setInterceptsMouseClicks (! on, ! on);
        padPanel.setArmMode (on);
    }
    void toggleMidiMapMode()
    {
        midiMapMode = ! midiMapMode;
        clearArm();
        if (midiMapMode) { keyMapMode = false; openMidiInputs(); }   // re-escanear por si conectaron el controlador
        setFadersArmable (midiMapMode);
        updateTransportEnabled();
        resized();
        repaint();
    }
    void refreshMapButtons()   // estado visual de los dos botones de mapping
    {
        keyMapBtn.setColour (juce::TextButton::buttonColourId, keyMapMode ? juce::Colour (0xff2E6BE6) : juce::Colour (0xff1f1f1f));
        keyMapBtn.setButtonText (keyMapMode ? juce::String::fromUTF8 ("Mapping de teclado \xe2\x9c\x93") : juce::String::fromUTF8 ("Mapping de teclado"));
        midiMapBtn.setColour (juce::TextButton::buttonColourId, midiMapMode ? juce::Colour (0xffB84BE6) : juce::Colour (0xff1f1f1f));
        midiMapBtn.setButtonText (midiMapMode ? juce::String::fromUTF8 ("MIDI Mapping \xe2\x9c\x93") : juce::String::fromUTF8 ("MIDI Mapping"));
        keyMapBtn.repaint(); midiMapBtn.repaint();
    }
    void loadMidiMap()
    {
        for (auto& v : actMidi) v = 0;
        midiTrackMute.clear(); midiTrackSolo.clear(); midiBusMute.clear(); midiBusSolo.clear();
        midiSong.clear(); midiTrackFader.clear(); midiBusFader.clear(); midiMasterFader = 0; midiPadFader = 0;
        auto v = juce::JSON::parse (npAppDir().getChildFile ("midimap.json"));
        if (auto* o = v.getDynamicObject())
        {
            for (int a = 0; a < kaCount && a < 16; ++a) actMidi[a] = (int) o->getProperty (juce::String (a));
            auto rd = [&o] (const char* k, std::map<juce::String,int>& mp)
            { if (auto* d = o->getProperty (k).getDynamicObject()) for (auto& pr : d->getProperties()) mp[pr.name.toString()] = (int) pr.value; };
            rd ("tmute", midiTrackMute); rd ("tsolo", midiTrackSolo);
            rd ("bmute", midiBusMute);   rd ("bsolo", midiBusSolo);
            rd ("tfader", midiTrackFader); rd ("bfader", midiBusFader);
            if (auto* d = o->getProperty ("songs").getDynamicObject())
                for (auto& pr : d->getProperties()) midiSong[pr.name.toString().getIntValue()] = (int) pr.value;
            midiMasterFader = (int) o->getProperty ("master");
            midiPadFader = (int) o->getProperty ("padfader");
        }
    }
    void saveMidiMap()
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        for (int a = 0; a < kaCount && a < 16; ++a) o->setProperty (juce::String (a), actMidi[a]);
        auto wr = [&o] (const char* k, std::map<juce::String,int>& mp)
        { juce::DynamicObject::Ptr d = new juce::DynamicObject(); for (auto& kv : mp) if (kv.second != 0) d->setProperty (kv.first, kv.second); o->setProperty (k, juce::var (d.get())); };
        wr ("tmute", midiTrackMute); wr ("tsolo", midiTrackSolo);
        wr ("bmute", midiBusMute);   wr ("bsolo", midiBusSolo);
        wr ("tfader", midiTrackFader); wr ("bfader", midiBusFader);
        juce::DynamicObject::Ptr sg = new juce::DynamicObject();
        for (auto& kv : midiSong) if (kv.second != 0) sg->setProperty (juce::String (kv.first), kv.second);
        o->setProperty ("songs", juce::var (sg.get()));
        o->setProperty ("master", midiMasterFader);
        o->setProperty ("padfader", midiPadFader);
        npAppDir().getChildFile ("midimap.json").replaceWithText (juce::JSON::toString (juce::var (o.get())));
    }
    static double ccToDb (int value) { return -60.0 + juce::jlimit (0, 127, value) / 127.0 * 60.0; }

    void onMidiMessage (const juce::MidiMessage& m)
    {
        if (midiMapMode)
        {
            if (armFader)                       // aprendiendo un fader continuo: necesita un CC
            {
                const int cc = midiCcCode (m);
                if (cc == 0) return;            // ignorar notas para faders
                const bool same = (armFaderIdx == -1 && midiMasterFader == cc)
                               || (armFaderIdx == -3 && midiPadFader == cc)
                               || (armFaderIdx == -2 && midiBusFader.count (armTrack) && midiBusFader[armTrack] == cc)
                               || (armFaderIdx >= 0  && armFaderIdx < trackNames.size() && midiTrackFader.count (trackNames[armFaderIdx]) && midiTrackFader[trackNames[armFaderIdx]] == cc);
                if (same) { if (armFaderIdx == -1) midiMasterFader = 0; else if (armFaderIdx == -3) midiPadFader = 0; else if (armFaderIdx == -2) midiBusFader[armTrack] = 0; else midiTrackFader[trackNames[armFaderIdx]] = 0; }
                else
                {
                    clearAllForMidi (cc);
                    if (armFaderIdx == -1) midiMasterFader = cc;
                    else if (armFaderIdx == -3) midiPadFader = cc;
                    else if (armFaderIdx == -2) midiBusFader[armTrack] = cc;
                    else midiTrackFader[trackNames[armFaderIdx]] = cc;
                }
                clearArm(); saveMidiMap(); refreshPadPanel(); repaint(); return;
            }
            if (armKind == 0) return;
            const int code = midiTrigCode (m);
            if (code == 0) return;              // ignorar mensajes que no son Note/CC
            const bool same =
                (armKind == 1 && armedAct >= 0 && actMidi[armedAct] == code)
             || (armKind == 2 && midiTrackMute.count (armTrack) && midiTrackMute[armTrack] == code)
             || (armKind == 4 && midiTrackSolo.count (armTrack) && midiTrackSolo[armTrack] == code)
             || (armKind == 5 && midiBusMute.count (armTrack)   && midiBusMute[armTrack]   == code)
             || (armKind == 6 && midiBusSolo.count (armTrack)   && midiBusSolo[armTrack]   == code)
             || (armKind == 3 && midiSong.count (armSong)       && midiSong[armSong]       == code);
            if (same)
            {
                if (armKind == 1) actMidi[armedAct] = 0;
                else if (armKind == 2) midiTrackMute[armTrack] = 0;
                else if (armKind == 4) midiTrackSolo[armTrack] = 0;
                else if (armKind == 5) midiBusMute[armTrack] = 0;
                else if (armKind == 6) midiBusSolo[armTrack] = 0;
                else if (armKind == 3) midiSong[armSong] = 0;
            }
            else
            {
                clearAllForMidi (code);
                if (armKind == 1) actMidi[armedAct] = code;
                else if (armKind == 2) midiTrackMute[armTrack] = code;
                else if (armKind == 4) midiTrackSolo[armTrack] = code;
                else if (armKind == 5) midiBusMute[armTrack] = code;
                else if (armKind == 6) midiBusSolo[armTrack] = code;
                else if (armKind == 3) midiSong[armSong] = code;
            }
            clearArm(); saveMidiMap(); repaint(); return;
        }

        // Modo normal: primero faders continuos (CC), luego disparadores
        if (m.isController())
        {
            const int cc = midiCcCode (m);
            const double db = ccToDb (m.getControllerValue());
            if (midiMasterFader == cc) { masterSlider.setValue (db, juce::sendNotificationSync); return; }
            if (midiPadFader == cc)    { padPanel.fader.setValue (db, juce::sendNotificationSync); return; }
            for (auto& kv : midiBusFader)
                if (kv.second == cc) { const int f = familyNames.indexOf (kv.first); if (f >= 0 && f < busSliders.size()) busSliders[f]->setValue (db, juce::sendNotificationSync); return; }
            for (auto& kv : midiTrackFader)
                if (kv.second == cc) { const int i = trackIndexForName (kv.first); if (i >= 0 && i < trackSliders.size()) trackSliders[i]->setValue (db, juce::sendNotificationSync); return; }
        }
        const bool press = (m.isNoteOn()) || (m.isController() && m.getControllerValue() >= 64);
        if (! press) return;
        const int code = midiTrigCode (m);
        if (code == 0) return;
        for (int a = 0; a < kaCount; ++a) if (actMidi[a] == code) { doAct (a); return; }
        for (auto& kv : midiTrackMute) if (kv.second == code) { const int i = trackIndexForName (kv.first); if (i >= 0) toggleTrackMute (i); return; }
        for (auto& kv : midiTrackSolo) if (kv.second == code) { const int i = trackIndexForName (kv.first); if (i >= 0) toggleTrackSolo (i); return; }
        for (auto& kv : midiBusMute)   if (kv.second == code) { const int f = familyNames.indexOf (kv.first); if (f >= 0) toggleBusMute (f); return; }
        for (auto& kv : midiBusSolo)   if (kv.second == code) { const int f = familyNames.indexOf (kv.first); if (f >= 0) toggleBusSolo (f); return; }
        for (auto& kv : midiSong)      if (kv.second == code) { selectSongById (kv.first); return; }
    }

    juce::Component* btnForAct (int a)
    {
        switch (a)
        {
            case kaPlay:      return &playButton;
            case kaReturn:    return &returnButton;
            case kaPrevBar:   return &barPrevBtn;
            case kaNextBar:   return &barNextBtn;
            case kaFade:      return &fadeButton;
            case kaLoop:      return &infiniteBtn;
            case kaRepeat:    return &repeatBtn;
            case kaPad:       return &padBtn;
            case kaBuses:     return &busesBtn;
            case kaMidi:      return &muteMidiBtn;
            case kaFaderView: return &faderViewBtn;
            case kaPadPlayer: return &padPlayerBtn;
        }
        return nullptr;
    }

    void toggleFade()
    {
        if (trackSliders.isEmpty()) return;
        fadedDown = ! fadedDown;
        if (fadedDown)
        {
            preFadeVals.clearQuick();
            for (auto* s : trackSliders) preFadeVals.add (s->getValue());
            fadeDir = -1;
        }
        else fadeDir = 1;
        fadeButton.setButtonText (fadedDown ? "Subir" : "Fade");
    }
    double snapToBeat (double sec) const   // cae en la barra de click (beat) más cercana
    {
        if (bpm <= 0.0) return sec;
        const double bl = 60.0 / bpm;
        if (clickSecArmed.load() && sec >= totalSeconds() - 1.0e-4)                 // en el bloque de click: pulsos parejos
            return totalSeconds() + std::round ((sec - totalSeconds()) / bl) * bl;

        const auto& g = currentBeatGrid;                                           // en la canción: grilla de beats (si hay)
        if (g.size() >= 2)
        {
            const int div = (gridBaseBpm > 0.0 && bpm > 0.0) ? juce::jlimit (1, 4, (int) std::llround (gridBaseBpm / bpm)) : 1;
            double best = g[0], bd = std::abs (sec - g[0]);
            for (int i = 0; i < g.size(); i += div) { const double d = std::abs (sec - g[i]); if (d < bd) { bd = d; best = g[i]; } }
            return best;
        }
        const double anchor = (sectionTimes.size() > 0 ? sectionTimes[0] : 0.0);   // respaldo: beats uniformes por BPM
        return juce::jmax (0.0, anchor + std::round ((sec - anchor) / bl) * bl);
    }
    void seekFromMouse (const juce::MouseEvent& e)
    {
        auto inner = mapBounds.reduced (8);
        if (! inner.contains (e.getPosition())) return;
        double vs = 0.0, ve = 0.0; getViewWindow (vs, ve);
        const double frac = juce::jlimit (0.0, 1.0, (double) (e.x - inner.getX()) / juce::jmax (1, inner.getWidth()));
        seekSeconds (snapToBeat (vs + frac * (ve - vs)));   // al soltar, cae en un click
    }
    void seekSeconds (double sec)
    {
        countInActive.store (false);   // #1 cualquier seek manual cancela el conteo
        const double fr = fileRates.isEmpty() ? 44100.0 : fileRates[0];
        seekTo.store ((long long) (juce::jmax (0.0, sec) * fr));
        repaint (mapBounds);
    }

    juce::String serverUrl, serverToken;
    juce::Array<SongEntry> repertoire;
    juce::Array<double> songMaster;   // master (dB) independiente por cancion
    juce::Array<juce::var> songMixCache;   // mezcla por cancion (del repertorio cargado)
    juce::Array<bool> songReady;      // audio de la canción ya descargado
    std::map<int, float> dlById;      // id de canción -> progreso 0..1 (ausente = sin barra). Sigue a la canción al reordenar
    juce::Array<int> loadOrderIds;    // ids en el ORDEN del loader (fijo); mapea el índice del loader al id aunque se reordene
    int pendingAddAfterId = 0;   // botón + de la tarjeta: insertar la canción agregada justo después de esta (0 = al final)
    int currentSong = -1;
    std::unique_ptr<RepertoireLoader> loader;
    std::unique_ptr<RepertoireLoader> offlineLoader;   // descarga de un repertorio para offline (no cambia la UI)
    juce::String offlineId;                            // repertorio que se está bajando para offline
    int offlineTotal = 0, offlinePct = -1;

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbCache { 1 };
    juce::AudioThumbnail thumb { 256, formatManager, thumbCache };
    juce::TimeSliceThread readThread { "np-read" };
    juce::OwnedArray<juce::AudioFormatReaderSource> readerSources;
    juce::OwnedArray<juce::BufferingAudioSource> bufferingSources;
    juce::OwnedArray<juce::ResamplingAudioSource> resamplers;
    juce::Array<double> fileRates;
    juce::StringArray trackNames;
    juce::StringArray trackServerFam;          // familia (del servidor) por track
    juce::StringArray curFamFiles, curFamNames;
    juce::Array<double> sectionTimes;
    juce::StringArray sectionNames;
    juce::Array<juce::File> stemFiles;
    double bpm = 0.0;
    double gridBaseBpm = 0.0;   // tempo base de la grilla (mediana) para escalar el BPM mostrado en medleys
    int beatsPerBar = 4;
    juce::String songCompas = "4/4";
    juce::AudioBuffer<float> temp;
    juce::Rectangle<int> mapBounds;
    juce::Rectangle<int> stripBounds;   // franja de tarjetas de canciones
    int stripScroll = 0;                // desplazamiento horizontal del strip
    juce::Rectangle<int> faderPanelBounds;
    bool splashOn = true;
    juce::uint32 splashStart = 0;
    int numTracks = 0;
    long long lengthSamples = 0;
    double deviceSampleRate = 44100.0;
    double preferredSampleRate = 0.0;   // 0 = automático (seguir la frecuencia del dispositivo)
    int currentBlockSize = 0;
    juce::CriticalSection graphLock;

    // ── Pads ambientales (structs PadVoice/PadPack declaradas más arriba) ──
    juce::CriticalSection padLock;                 // protege padVoices (aparte de graphLock)
    juce::OwnedArray<PadVoice> padVoices;          // 1-2 voces (crossfade al cambiar de tono)
    juce::AudioBuffer<float> padTemp;
    std::atomic<bool>  padEnabled { false };
    std::atomic<float> padGain { 1.0f };           // fader del pad (0 dB por defecto)
    std::atomic<int>   padPlayingIdx { -1 };       // tono objetivo que suena (0-11), -1 = ninguno
    std::atomic<int>   padJobGen { 0 };            // generación: descarta descargas viejas
    double padXfadeSec = 3.0;                       // duración del fade/crossfade (s)
    float  padGainCur = 1.0f;                        // ganancia suavizada del fader (anti-zipper)
    int    padMode = 0;                             // 0 = Auto (sigue la canción), 1 = Manual
    int    padManualIdx = 0;                        // tono elegido a mano (0-11)
    double padGainDb = 0.0;
    std::atomic<int>   padDbgVoices { 0 };   // diagnóstico temporal
    std::atomic<float> padDbgMg { 0.0f };
    std::atomic<float> padDbgAbs { 0.0f };
    juce::String padPackId, padPackName, padPackPortadaRel;
    int    padPackBaseIdx = 0;
    juce::Array<PadPack> padPacks;                  // catálogo del servidor
    juce::Image padPortadaImg;
    PadPanel padPanel;                              // vista de faders del pad (faderView 3)
    std::atomic<int> padReadyMask { 0 };            // bits de tonos ya descargados
    std::atomic<int> padPrefetchGen { 0 };          // generación de la precarga de los 12 tonos
    int midiPadFader = 0;                            // CC MIDI asignado al fader del pad (0 = ninguno)
    // Pad Player por canción (intro / outro automáticos)
    std::map<int, std::pair<bool,bool>> songPad;    // songId -> {intro, outro}
    std::atomic<bool>  curPadIntro { false };        // flags de la canción actual
    std::atomic<bool>  curPadOutro { false };
    std::atomic<float> padAutoGain { 1.0f };         // ganancia de automatización (intro/outro)
    float  padAutoCur = 1.0f;                        // suavizado de padAutoGain
    bool   padAutoActive = false;                    // la automatización controla el pad ahora
    bool   padAutoTurnedOn = false;                  // la automatización fue quien encendió el pad
    bool   padAutoFadeOutStarted = false;            // ya se disparó el fade-out del intro
    bool   padOutroLatched = false;                  // outro enganchado: se mantiene aunque vuelva al inicio
    bool   padUserOverride = false;                   // el usuario tomó el control manual (suspende la automatización en la zona)
    bool   padPrevPlaying = false;                     // para detectar el flanco de arranque de reproducción
    double padIntroInSec  = 3.0;                     // fade-in del pad al iniciar
    double padIntroOutSec = 3.0;                     // fade-out del pad al iniciar
    double padOutroFadeSec = 6.0;                    // fade-in del pad de outro

    bool browsing = false, isDragging = false, dragSeeks = false;
    bool lastPlaying = false;
    double dragStartCenter = 0.0, browseCenter = 0.0;
    juce::uint32 lastInteractionMs = 0;

    std::atomic<bool> prepared { false };
    std::atomic<bool> playing { false };
    std::atomic<bool> loadingSong { false };
    std::atomic<long long> positionOut { 0 };
    std::atomic<long long> seekTo { -1 };

    static constexpr int kMaxTracks = 32;
    std::atomic<float> trackGain[kMaxTracks];
    std::atomic<bool> trackMuted[kMaxTracks];
    std::atomic<bool> trackSolo[kMaxTracks];
    std::atomic<float> trackLevel[kMaxTracks];
    // Ajustes de mezcla compartida (menú)
    bool masterPerSong = true;                          // master independiente por canción vs general del setlist
    double globalMasterDb = 0.0;                        // master general (cuando masterPerSong = false)
    bool mixPerSong = true;                             // buses+mute independientes por canción vs generales
    std::map<juce::String,double> globalBusGain;        // familia -> dB (mezcla general de buses)
    std::set<juce::String> globalMutedFamilies;         // familias (buses) silenciadas en la mezcla general
    std::set<juce::String> globalMuted;                 // canales sueltos silenciados en la mezcla general (por nombre)
    bool suppressGlobalSave = false;                    // no pisar la mezcla general durante cambios programáticos
    bool mixDirty = false;                              // hay cambios de mezcla sin guardar (puntito rojo en Repertorios)
    bool trackNoFade[kMaxTracks] = { false };          // #1 Click/Guía NO suben en el conteo (para oír las indicaciones)
    bool countInEnabled = false;                        // #1 toggle del usuario (menú de opciones)
    std::atomic<bool> countInActive { false };          // conteo en curso
    std::atomic<double> countInStartSec { 0.0 };        // inicio del swell
    std::atomic<double> countInEndSec { 0.0 };          // downbeat de la sección (llega a nivel normal aquí)
    std::atomic<float> masterGain { 1.0f };

    std::atomic<float> busGain[16];         // ganancia por familia (bus)
    int trackFamily[kMaxTracks] = { 0 };    // familia (bus) de cada track
    int faderView = 0;                      // 0 = tracks, 1 = buses
    int vuTick = 0;                         // para refrescar el VU a la mitad de FPS

    std::atomic<bool> loopActive { false };     // infinito (permanente)
    std::atomic<bool> loopOnce { false };       // repetir una vez
    std::atomic<double> loopStartSec { -1.0 };
    std::atomic<double> loopEndSec { -1.0 };

    // #2 punto de inicio/fin por canción (-1 = sin definir)
    std::map<int, std::pair<double,double>> songInOut;   // id de canción -> {inicio, fin}
    std::atomic<double> songInSec  { -1.0 };
    std::atomic<double> songOutSec { -1.0 };

    // #3 sección de click: bloque de 2 compases DESPUÉS del final, con metrónomo sintetizado
    bool trackIsClick[kMaxTracks] = { false };
    std::set<int> clickSecSongs;                 // ids de canciones con sección de click (persistente)
    bool songHasClickSec = false;                // la canción actual tiene sección de click
    std::atomic<bool> clickSecArmed { false };   // hay sección de click (para el hilo de audio)
    std::atomic<bool> clickLoopOn { false };     // ∞ del bloque de click (por defecto ON al agregar)
    std::atomic<double> clickLenSec { 0.0 };     // largo del bloque (2 compases)
    juce::Rectangle<int> addClickBtnRect;        // + al final de la última sección (modo edición)
    juce::Rectangle<int> delClickBtnRect;        // − sobre el bloque de click (modo edición)
    // metrónomo sintetizado para la sección de click (el click grabado suele acabar antes del final)
    double clkEnv = 0.0, clkPhase = 0.0, clkFreq = 1200.0;
    long long clkLastBeat = -1;

    // #4 mapping de teclado
    enum KMAct { kaPlay = 0, kaReturn, kaPrevBar, kaNextBar, kaFade, kaLoop, kaRepeat,
                 kaPad, kaBuses, kaMidi, kaFaderView, kaPadPlayer, kaCount };
    int  actKey[kaCount] = { 0 };               // código de tecla por acción fija (0 = sin asignar)
    std::map<juce::String,int> keyByTrack;      // nombre de track (MUTE) -> tecla
    std::map<juce::String,int> keyBySolo;       // nombre de track (SOLO) -> tecla
    std::map<juce::String,int> keyByBusMute;    // familia/bus (MUTE) -> tecla
    std::map<juce::String,int> keyByBusSolo;    // familia/bus (SOLO) -> tecla
    std::map<int,int> keyBySong;                // id de canción (bloque) -> tecla
    bool keyMapMode = false;                    // modo de asignación de teclas
    // arming: kind 0=nada,1=acción,2=mute,3=canción,4=solo,5=busMute,6=busSolo
    int armKind = 0, armedAct = -1, armSong = -1;
    juce::String armTrack;
    bool editBarOpen = false;                   // barra desplegable de Editar
    juce::Rectangle<int> editBarBounds;         // fondo de la barra de Editar

    // #5/#6 MIDI IN + learn (paralelo al mapping de teclado)
    juce::OwnedArray<juce::MidiInput> midiInputs;   // todas las entradas MIDI abiertas
    bool midiMapMode = false;                        // modo "MIDI Mapping"
    bool armFader = false;                           // se armó un fader (control continuo), no un disparador
    int  armFaderIdx = -1;                           // -1=master, >=0 track, o bus por nombre en armTrack
    juce::TextButton midiMapBtn;                     // botón "MIDI Mapping" en la barra de Editar
    // disparadores (Note/CC-botón) — códigos MIDI, mismos elementos que el teclado
    int  actMidi[16] = { 0 };                        // por acción fija (kaCount<=16)
    std::map<juce::String,int> midiTrackMute, midiTrackSolo, midiBusMute, midiBusSolo;
    std::map<int,int> midiSong;
    // faders continuos (CC -> dB)
    std::map<juce::String,int> midiTrackFader, midiBusFader;   // nombre -> código CC
    int  midiMasterFader = 0;

    juce::Image logoImg;
    PillLNF pillLnf;
    FaderLNF faderLnf;
    juce::TextButton connectButton, returnButton, barPrevBtn, barNextBtn;
    PlayIconButton playButton;
    FadeIconButton fadeButton;
    juce::Array<double> preFadeVals;
    int fadeDir = 0;            // -1 bajando, +1 subiendo, 0 quieto
    bool fadedDown = false;
    juce::Label connStatus, timeLabel, masterLabel;
    juce::Slider masterSlider;
    juce::TextButton busesBtn, padPlayerBtn, muteMidiBtn, editBtn, padBtn, keyMapBtn;
    IconButton faderViewBtn, repeatBtn, infiniteBtn, settingsBtn, repertoireBtn;
    FaderStripComp faderStrip;
    HScrollViewport faderViewport;
    MidiPanel midiPanel;
    RepertoirePicker repPicker;
    SettingsPanel settingsPanel;
    StoragePanel storagePanel;
    bool cacheAutoClean = false;
    int  cacheCapGB = 0;
    bool didStartupClean = false;
    juce::String lastSetlistId;   // setlist cargado (para "Actualizar")
    juce::String currentSetlistName;
    int loadGen = 0;                 // generación de carga: descarta callbacks de cargas canceladas
    juce::Rectangle<int> setlistBandBounds;   // franja donde se dibuja el nombre del repertorio
    juce::Rectangle<int> compasBoxBounds;     // caja de Tempo/Compás (a la par del tiempo)
    RepEditPanel repEdit;
    AddCard addCard;
    bool editMode = false;
    juce::Array<RepEditPanel::BibItem> bibliotecaAll;
    bool syncEnabled = false;
    std::atomic<bool> syncLinked { false };
    int syncPingCtr = 0, syncPollCtr = 0;
    HttpLiveServer liveServer;
    juce::String currentChartJson { "{}" };
    juce::String perfilesJson { "[]" };            // roster de perfiles (cache) para el visor local
    juce::CriticalSection chartLock;
    std::atomic<int> liveSectionIdx { 0 };
    std::atomic<int> liveSongVer { 0 };
    AudioConfigPanel audioCfg;
    juce::String audioOutDevice;
    std::map<juce::String, std::array<AudioConfigPanel::FamRoute, kNumFam>> routesByDevice;
    int famMode[kNumFam] = { 2,2,2,2,2,2,2,2,2,2,2 };   // snapshot para el hilo de audio (default estéreo)
    int famBaseCh[kNumFam] = { 0 };
    int trackRouteFam[kMaxTracks] = { 0 };
    int openOutChans = 2;
    juce::Array<MidiBox> currentMidiBoxes;
    juce::Array<double> currentBeatGrid;           // negras (seg) de la canción actual, para el MIDI clock
    juce::OwnedArray<juce::MidiOutput> midiOuts;
    juce::Array<juce::MidiOutput*> cajaOut;
    juce::MidiOutput* midiClockOutPtr = nullptr;   // salida del MIDI Clock (sin canal)
    bool clockEnabledFlag = false, clockRunning = false;
    long long clockPulses = 0;
    int midiNext[8] = { 0 };
    double midiCursor = 0.0;
    struct POff { juce::MidiOutput* out = nullptr; int chan = 1; int note = 0; juce::uint32 t = 0; };
    juce::Array<POff> midiOffs;
    juce::CriticalSection midiLock;
    bool cajaOnArr[8] = { false };
    int  cajaChanArr[8] = { 1,1,1,1,1,1,1,1 };
    struct MidiClock : public juce::HighResolutionTimer
    {
        std::function<void()> tick;
        void hiResTimerCallback() override { if (tick) tick(); }
    } midiClock;
    juce::OwnedArray<SongCard> songCards;
    juce::OwnedArray<juce::Slider> trackSliders;
    juce::OwnedArray<ClickLabel> trackLabels;
    juce::OwnedArray<SoloDot> soloDots;
    juce::OwnedArray<juce::Slider> busSliders;
    juce::OwnedArray<ClickLabel> busLabels;
    juce::OwnedArray<SoloDot> busSoloDots;
    juce::StringArray familyNames;
    juce::Array<int> faderOrder;
    int numSpecialFaders = 0;
    int faderSepX = -1;
    int masterSepX = -1;
    std::unique_ptr<MixThumb> mixBuilder;
    juce::Image waveImg;
    bool waveDirty = true;
    double wavePps = 60.0;
    SplashComp splash;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class NeuralPlayApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "NeuralPlay"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow.reset (new MainWindow ("NeuralPlay", new MainComponent()));
    }
    void shutdown() override { mainWindow = nullptr; }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (juce::String name, juce::Component* c)
            : DocumentWindow (name, juce::Colour (0xff0a0a0a), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (c, true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (NeuralPlayApplication)

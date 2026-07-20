#include <JuceHeader.h>
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
static bool httpDownload (const juce::String& url, const juce::String& token, const juce::File& dest)
{
    juce::URL u (url);
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withExtraHeaders ("Authorization: Bearer " + token)
                    .withConnectionTimeoutMs (30000);
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr) return false;
    dest.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp (dest);
    {
        std::unique_ptr<juce::FileOutputStream> out (tmp.getFile().createOutputStream());
        if (out == nullptr) return false;
        out->writeFromInputStream (*in, -1);
    }
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
    juce::String titulo, artista, tonoNombre;
    juce::File folder;
    juce::Array<double> secTimes;
    juce::StringArray secNames;
    juce::StringArray famFiles, famNames;   // familia por stem (del servidor)
    juce::Array<MidiBox> midiBoxes;         // cajas MIDI + notas (del servidor)
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

struct SongCard : public juce::Component
{
    juce::Image cover;
    juce::String titulo, tono;
    bool active = false;
    bool editMode = false;
    std::function<void()> onClick, onRemove, onTono;

    juce::Rectangle<float> coverRect() const
    {
        auto r = getLocalBounds().toFloat();
        r.removeFromTop (6.0f);
        return r.removeFromTop (r.getHeight() * 0.76f).reduced (1.0f);
    }
    juce::Rectangle<float> removeBtnRect() const { auto c = coverRect(); return { c.getRight() - 34.0f, c.getY() + 8.0f, 26.0f, 26.0f }; }
    juce::Rectangle<float> tonoBtnRect()   const { auto c = coverRect(); return { c.getCentreX() - 22.0f, c.getCentreY() - 18.0f, 44.0f, 36.0f }; }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mouseWasClicked()) return;
        if (editMode)
        {
            if (removeBtnRect().contains (e.position)) { if (onRemove) onRemove(); return; }
            if (tonoBtnRect().contains (e.position))   { if (onTono)   onTono();   return; }
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
    if (C("drum")||C("bater")||C("perc")||C("loop")||C("beat")||C("kick")||C("snare")||C("hat")||C("tom")) return juce::String::fromUTF8 ("Bater\xc3\xad" "a");
    if (C("guit")||C("gtr")||C("guitar")|| tok=="ag" || tok=="eg" || tok=="ge") return juce::String::fromUTF8 ("Guitarras");
    if (C("key")||C("teclad")||C("piano")||C("synth")||C("rhodes")||C("organ")||C("pad")||C("string")|| tok=="kb") return juce::String::fromUTF8 ("Teclados");
    if (C("voz")||C("vocal")||C("coro")||C("lead")||C("bgv")||C("choir")||C("voc")) return juce::String::fromUTF8 ("Voces");
    return juce::String::fromUTF8 ("Otros");
}

// ───────── Enrutamiento de salidas de audio por familia ─────────
static const char* kRouteFam[11] = {
    "Voces", "Guitarras", "Teclados", "Cuerdas", "Metales", "Bajo",
    "Percusi\xc3\xb3n", "Gu\xc3\xad" "a", "M\xc3\xbasica original", "Click", "Otros" };
static constexpr int kNumFam = 11;

static int routeFamIndex (const juce::String& serverFam, const juce::String& trackName)
{
    juce::String fam = serverFam;
    if (fam.isEmpty()) fam = familyFor (trackName);
    if (fam.startsWithIgnoreCase ("Bater")) return 6;   // Batería -> Percusión
    for (int i = 0; i < kNumFam; ++i)
        if (fam.equalsIgnoreCase (juce::String::fromUTF8 (kRouteFam[i]))) return i;
    return 10;   // Otros
}

struct FaderStripComp : public juce::Component
{
    std::function<void (juce::Graphics&)> onPaint;
    void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
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
    std::function<void (juce::Array<SongEntry>)> onDone;

    void status (const juce::String& s)
    {
        if (onStatus) { auto cb = onStatus; juce::MessageManager::callAsync ([cb, s] { cb (s); }); }
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
        auto* cs = sl.getProperty ("canciones", juce::var()).getArray();
        if (cs == nullptr || cs->isEmpty()) { status ("Repertorio vacio"); return; }

        juce::Array<SongEntry> out;
        for (int i = 0; i < cs->size(); ++i)
        {
            if (threadShouldExit()) return;
            auto item = (*cs)[i];
            SongEntry e;
            e.id   = (int) item.getProperty ("id", 0);
            e.tono = (int) item.getProperty ("tono_semitonos", 0);   // semitono resuelto por el servidor
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
                    if (threadShouldExit()) return;
                    auto fn = st.getProperty ("file", "").toString();
                    if (fn.isEmpty()) continue;
                    e.famFiles.add (fn);
                    e.famNames.add (st.getProperty ("familia", "").toString());
                    auto dest = e.folder.getChildFile (fn);
                    if (! dest.existsAsFile() || dest.getSize() < 2000)
                    {
                        status ("Descargando: " + e.titulo + "  -  " + fn);
                        httpDownload (serverUrl + "/api/live/pista/" + juce::String (e.id) + "/"
                                      + juce::URL::addEscapeChars (fn, false)
                                      + "?t=" + juce::String (e.tono), token, dest);
                    }
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
            out.add (e);
        }
        status ("Repertorio listo: " + slName);
        if (onDone) { auto cb = onDone; juce::MessageManager::callAsync ([cb, out] { cb (out); }); }
    }

    juce::String serverUrl, token;
    juce::String wantedId;    // setlist elegido (vacio = el primero)
    juce::String resolvedId;  // id real del setlist cargado (lo llena run())
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
    };
    juce::OwnedArray<Row> rows;
    juce::File cfgFile;
    std::function<void()> onChanged;

    MidiPanel()
    {
        static const char* nm[8]  = { "Lyrics","Lights 1","Lights 2","Patches 1","Patches 2","Guitar","Aux 1","Aux 2" };
        static const char* ids[8] = { "lyrics","lights1","lights2","patches1","patches2","guitar","aux1","aux2" };
        static const int   chd[8] = { 16,1,2,3,4,5,6,7 };
        for (int i = 0; i < 8; ++i)
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
            g.setColour (cajaColour (i));
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
            r->chan.setBounds (row.removeFromRight (120));
            row.removeFromRight (12);
            r->port.setBounds (row);
        }
    }

    bool isOn (int i) const { return i >= 0 && i < rows.size() && rows[i]->on.getToggleState(); }
    int  channel (int i) const { return (i >= 0 && i < rows.size()) ? rows[i]->chan.getSelectedId() : 1; }
    juce::String portName (int i) const { return (i >= 0 && i < rows.size() && rows[i]->port.getSelectedId() > 1) ? rows[i]->port.getText() : juce::String(); }
    int count() const { return rows.size(); }

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
    struct Item { juce::String id, nombre, fecha; int nCanciones = 0; };
    juce::Array<Item> items;
    int selected = -1;
    bool loading = true;
    std::function<void (juce::String)> onLoad;
    std::function<void()> onNew;
    juce::TextButton loadBtn, newBtn, closeBtn;

    RepertoirePicker()
    {
        loadBtn.setButtonText ("Cargar");
        loadBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffffffff));
        loadBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff0a0a0a));
        loadBtn.onClick = [this]
        {
            if (selected >= 0 && selected < items.size() && onLoad)
            { setVisible (false); onLoad (items[selected].id); }
        };
        addAndMakeVisible (loadBtn);
        newBtn.setButtonText (juce::String::fromUTF8 ("+ Nuevo"));
        newBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        newBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff7Cc6ff));
        newBtn.onClick = [this] { setVisible (false); if (onNew) onNew(); };
        addAndMakeVisible (newBtn);
        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        closeBtn.onClick = [this] { setVisible (false); };
        addAndMakeVisible (closeBtn);
        setAlwaysOnTop (true);
    }

    juce::Rectangle<int> panelBounds() const
    {
        const int w = 430;
        const int h = 150 + juce::jmax (1, items.size()) * 54;
        return getLocalBounds().withSizeKeepingCentre (w, juce::jmin (h, getHeight() - 80));
    }
    juce::Rectangle<int> rowBounds (int i) const
    {
        auto p = panelBounds();
        return { p.getX() + 20, p.getY() + 64 + i * 54, p.getWidth() - 40, 48 };
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xC0000000));   // velo
        auto p = panelBounds().toFloat();
        g.setColour (juce::Colour (0xff141414));
        g.fillRoundedRectangle (p, 14.0f);
        g.setColour (juce::Colour (0x33ffffff));
        g.drawRoundedRectangle (p, 14.0f, 1.2f);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (17.0f, juce::Font::bold));
        g.drawText ("Abrir repertorio", panelBounds().removeFromTop (56).reduced (22, 0),
                    juce::Justification::centredLeft);

        if (loading)
        {
            g.setColour (juce::Colour (0xffa3a3a3));
            g.setFont (juce::Font (13.0f));
            g.drawText ("Buscando repertorios...", panelBounds().reduced (20), juce::Justification::centred);
        }
        else if (items.isEmpty())
        {
            g.setColour (juce::Colour (0xffa3a3a3));
            g.setFont (juce::Font (13.0f));
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
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (14.0f, juce::Font::bold));
            g.drawText (items[i].nombre, r.reduced (14, 6).removeFromTop (18.0f), juce::Justification::centredLeft);
            g.setColour (juce::Colour (0xffa3a3a3));
            g.setFont (juce::Font (11.5f));
            g.drawText (items[i].fecha + "   \xc2\xb7   " + juce::String (items[i].nCanciones) + " canciones",
                        r.reduced (14, 6).removeFromBottom (16.0f), juce::Justification::centredLeft);
        }
    }

    void resized() override { layoutButtons(); }
    void layoutButtons()
    {
        auto p = panelBounds();
        closeBtn.setBounds (p.getRight() - 46, p.getY() + 12, 34, 30);
        auto brow = juce::Rectangle<int> (p.getX() + 20, p.getBottom() - 54, p.getWidth() - 40, 36);
        newBtn.setBounds (brow.removeFromLeft (110));
        loadBtn.setBounds (brow.removeFromRight (130));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < items.size(); ++i)
            if (rowBounds (i).contains (e.getPosition())) { selected = i; repaint(); return; }
        if (! panelBounds().contains (e.getPosition())) setVisible (false);   // clic fuera cierra
    }

    void setItems (juce::Array<Item> it)
    {
        items = std::move (it);
        loading = false;
        if (selected < 0 && ! items.isEmpty()) selected = 0;
        layoutButtons();
        repaint();
    }
};

struct SettingsPanel : public juce::Component
{
    juce::TextButton syncBtn, cfgBtn, refreshBtn, closeBtn;
    bool syncOn = false, linked = false;
    juce::Rectangle<int> indRect;
    std::function<void (bool)> onSync;
    std::function<void()> onConfig;
    std::function<void()> onRefresh;

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

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        closeBtn.onClick = [this] { setVisible (false); };
        addAndMakeVisible (closeBtn);

        setAlwaysOnTop (true);
        refresh();
    }

    void setState (bool on, bool lk) { syncOn = on; linked = lk; refresh(); }

    void refresh()
    {
        syncBtn.setButtonText (syncOn ? "Sincronizado" : "Sincronizar");
        syncBtn.setColour (juce::TextButton::buttonColourId,
                           syncOn ? juce::Colour (0xff17361f) : juce::Colour (0xff1f1f1f));
        syncBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        repaint();
    }

    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withSizeKeepingCentre (360, 312); }

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

        // check-in verde del puente
        auto r = indRect.toFloat();
        g.setColour (linked ? juce::Colour (0xff3ED66E) : juce::Colour (0xff3a3a3a));
        g.fillEllipse (r);
        if (linked)
        {
            g.setColour (juce::Colour (0xff0a0a0a));
            g.setFont (juce::Font (11.0f, juce::Font::bold));
            g.drawText (juce::String::fromUTF8 ("\xe2\x9c\x93"), r, juce::Justification::centred);
        }
        g.setColour (juce::Colour (0xffa3a3a3));
        g.setFont (juce::Font (11.0f));
        g.drawText (linked ? "enlazado" : (syncOn ? "esperando..." : "sin enlazar"),
                    juce::Rectangle<float> (r.getX() - 96.0f, r.getCentreY() - 8.0f, 90.0f, 16.0f),
                    juce::Justification::centredRight);
    }

    void resized() override
    {
        auto p = panelBounds();
        closeBtn.setBounds (p.getRight() - 46, p.getY() + 12, 34, 30);
        auto b = p.reduced (24); b.removeFromTop (44);
        auto row = b.removeFromTop (48);
        indRect = row.removeFromRight (18).withSizeKeepingCentre (16, 16);
        row.removeFromRight (10);
        syncBtn.setBounds (row);
        b.removeFromTop (14);
        cfgBtn.setBounds (b.removeFromTop (48).withTrimmedRight (28));
        b.removeFromTop (14);
        refreshBtn.setBounds (b.removeFromTop (48).withTrimmedRight (28));
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
    juce::TextButton closeBtn;
    int numChans = 2;
    std::function<void (const juce::String&)> onDevice;
    std::function<void (int, int, int)> onRoute;   // fam, mode, base

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
        numChans = juce::jlimit (0, 12, chans);
        chInfo.setText (juce::String (numChans) + juce::String::fromUTF8 (" canales disponibles"), juce::dontSendNotification);
        for (auto* c : routeBoxes)
        {
            c->clear (juce::dontSendNotification);
            c->addItem ("Off", 1);
            for (int k = 1; k <= numChans; ++k)          c->addItem (juce::String (k), 100 + k);                                  // mono
            for (int k = 1; k + 1 <= numChans; k += 2)   c->addItem (juce::String (k) + "/" + juce::String (k + 1), 200 + k);     // estéreo
        }
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

    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withSizeKeepingCentre (480, 560); }

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
        auto b = p.reduced (22);
        title.setBounds (b.removeFromTop (34));
        b.removeFromTop (4);
        devLbl.setBounds (b.removeFromTop (16));
        deviceBox.setBounds (b.removeFromTop (30));
        chInfo.setBounds (b.removeFromTop (20));
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

    struct BibItem { int id = 0; juce::String titulo, tono; };
    juce::Array<BibItem> bib;

    int songId = 0; juce::String songTitle; bool addFlow = false;
    struct Key { juce::String nombre; int sem = 0; bool rendered = false; };
    juce::Array<Key> keys;
    int renderingSem = 99, pendIdx = -1, progHechos = 0, progTotal = 0;

    juce::TextButton closeBtn;
    std::function<void (int)> onPickSong;            // biblioteca -> elegir cancion
    std::function<void (int, juce::String)> onChoose; // (semitonos, nombre) -> aplicar

    RepEditPanel()
    {
        setAlwaysOnTop (true);
        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        closeBtn.onClick = [this] { stopTimer(); renderingSem = 99; setVisible (false); };
        addAndMakeVisible (closeBtn);
    }

    void openBiblioteca (juce::Array<BibItem> items)
    { mode = Biblioteca; bib = std::move (items); renderingSem = 99; stopTimer(); resized(); repaint(); }
    void openTono (int id, juce::String title, bool add, juce::Array<Key> ks)
    { mode = Tono; songId = id; songTitle = title; addFlow = add; keys = std::move (ks); renderingSem = 99; stopTimer(); resized(); repaint(); }

    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withSizeKeepingCentre (470, 520); }

    juce::Rectangle<int> keyRect (int i) const
    {
        auto p = panelBounds().reduced (22); p.removeFromTop (58);
        const int cols = 3, gap = 10, cw = (p.getWidth() - (cols - 1) * gap) / cols, ch = 60;
        return { p.getX() + (i % cols) * (cw + gap), p.getY() + (i / cols) * (ch + gap), cw, ch };
    }
    juce::Rectangle<int> bibRect (int i) const
    {
        auto p = panelBounds().reduced (18); p.removeFromTop (52);
        return { p.getX(), p.getY() + i * 48, p.getWidth(), 42 };
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
        g.drawText (title, panelBounds().removeFromTop (52).reduced (22, 0), juce::Justification::centredLeft);

        if (mode == Biblioteca)
        {
            for (int i = 0; i < bib.size(); ++i)
            {
                auto r = bibRect (i).toFloat();
                g.setColour (juce::Colour (0xff1c1c1c)); g.fillRoundedRectangle (r, 9.0f);
                g.setColour (juce::Colour (0x22ffffff)); g.drawRoundedRectangle (r, 9.0f, 1.0f);
                g.setColour (juce::Colours::white); g.setFont (juce::Font (14.0f, juce::Font::bold));
                g.drawText (bib[i].titulo, r.reduced (14, 0).removeFromTop (r.getHeight() * 0.62f), juce::Justification::centredLeft);
                g.setColour (juce::Colour (0xffa3a3a3)); g.setFont (juce::Font (11.5f));
                g.drawText (juce::String::fromUTF8 ("Tono ") + bib[i].tono, r.reduced (14, 5).removeFromBottom (15.0f), juce::Justification::centredLeft);
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
                g.setFont (juce::Font (19.0f, juce::Font::bold));
                g.drawText (k.nombre, r.withTrimmedBottom (k.rendered ? 0.0f : 14.0f), juce::Justification::centred);
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
        }
    }

    void resized() override { closeBtn.setBounds (panelBounds().getRight() - 46, panelBounds().getY() + 12, 34, 30); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelBounds().contains (e.getPosition())) { stopTimer(); renderingSem = 99; setVisible (false); return; }
        if (renderingSem != 99) return;   // ocupado renderizando
        if (mode == Biblioteca)
        {
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
 :root{--bg:#0a0a0a;--panel:#141414;--txt:#f2f2f2;--txt2:#a3a3a3;--line:#262626;--chord:#E6C15A;--active:#3ED66E;}
 *{box-sizing:border-box}
 html,body{margin:0;height:100%;background:var(--bg);color:var(--txt);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;-webkit-text-size-adjust:100%}
 .top{position:sticky;top:0;z-index:5;background:rgba(10,10,10,.92);backdrop-filter:blur(8px);padding:12px 16px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:12px}
 .title{font-size:18px;font-weight:800;line-height:1.15}
 .meta{font-size:12px;color:var(--txt2)}
 .tono{margin-left:auto;font-size:13px;font-weight:700;color:var(--chord);border:1px solid var(--line);border-radius:999px;padding:4px 12px;white-space:nowrap}
 .live{width:9px;height:9px;border-radius:50%;background:#444;display:inline-block;margin-right:7px;vertical-align:middle}
 .live.on{background:var(--active);box-shadow:0 0 8px var(--active)}
 .wrap{padding:14px 16px 55vh}
 .sec{border:1px solid var(--line);border-radius:12px;padding:14px 14px 12px;margin:0 0 12px;opacity:.45;transition:opacity .2s,border-color .2s,box-shadow .2s}
 .sec.active{opacity:1;border-color:var(--active);box-shadow:0 0 0 1px var(--active)}
 .sechead{font-size:13px;font-weight:800;text-transform:uppercase;letter-spacing:.06em;color:var(--txt2);margin-bottom:8px}
 .sec.active .sechead{color:var(--active)}
 .secnote{font-size:12px;color:var(--txt2);margin:-4px 0 8px;font-style:italic}
 .lyrics{display:flex;flex-direction:column;gap:6px}
 .line{display:flex;flex-wrap:wrap;align-items:flex-end}
 .tok{display:flex;flex-direction:column;margin-right:2px}
 .chord{color:var(--chord);font-weight:700;font-size:15px;min-height:18px;white-space:pre}
 .lyric{font-size:17px;white-space:pre}
 .inst{display:flex;flex-wrap:wrap;gap:8px}
 .chip{border:1px solid var(--line);border-radius:8px;padding:5px 10px;color:var(--chord);font-weight:700;font-size:15px}
 .inst-label{font-size:12px;color:var(--txt2);margin-bottom:6px}
 .off{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;color:var(--txt2);font-size:15px;padding:20px;text-align:center;background:var(--bg)}
</style></head><body>
<div class="top"><div style="min-width:0"><div class="title"><span class="live" id="lv"></span><span id="t">—</span></div><div class="meta" id="m"></div></div><div class="tono" id="k"></div></div>
<div class="wrap" id="wrap"></div>
<div class="off" id="off">Esperando al reproductor…</div>
<script>
let song=null, ver=-1, idx=-1;
function esc(s){return String(s==null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function renderLines(lines){return '<div class="lyrics">'+(lines||[]).map(function(line){return '<div class="line">'+(line||[]).map(function(t){return '<div class="tok"><span class="chord">'+esc(t[0])+'</span><span class="lyric">'+esc((t[1]==null||t[1]==="")?" ":t[1])+'</span></div>'}).join('')+'</div>'}).join('')+'</div>'}
function renderBody(s){ if(s.inst&&s.prog){return '<div class="inst-label">Instrumental</div><div class="inst">'+s.prog.map(function(a){return '<span class="chip">'+esc(a)+'</span>'}).join('')+'</div>'} return renderLines(s.lines)}
function renderSong(){
  document.getElementById('t').textContent=song.titulo||'—';
  document.getElementById('m').textContent=(song.tempo?song.tempo+' BPM':'')+(song.compas?'   ·   '+song.compas:'');
  document.getElementById('k').textContent=song.tono||'';
  document.getElementById('wrap').innerHTML=(song.secciones||[]).map(function(s,i){return '<div class="sec" id="sec-'+i+'"><div class="sechead">'+esc(s.tipo)+'</div>'+(s.nota?'<div class="secnote">'+esc(s.nota)+'</div>':'')+renderBody(s)+'</div>'}).join('');
  idx=-1;
}
function setActive(i){
  if(!song||!song.secciones||i<0||i>=song.secciones.length||i===idx) return;
  idx=i;
  document.querySelectorAll('.sec').forEach(function(el,si){el.classList.toggle('active',si===i)});
  var target=document.getElementById('sec-'+i);
  if(target){ window.scrollTo({top:target.getBoundingClientRect().top+window.scrollY-92,behavior:'smooth'}); }
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
setInterval(tick,400); tick();
</script></body></html>)HTMLPAGE";

// ───────── Servidor HTTP embebido (músicos se conectan a http://<ip-mac>:5050) ─────────
struct HttpLiveServer : private juce::Thread
{
    HttpLiveServer() : juce::Thread ("LiveHTTP") {}
    ~HttpLiveServer() override { stop(); }

    int port = 5050;
    std::function<juce::String()> getPage, getSong, getState;
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

class MainComponent : public juce::AudioAppComponent,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    MainComponent()
    {
        loadConfig();
        setLookAndFeel (&pillLnf);
        { auto lf = npAppDir().getChildFile ("logo.png"); if (lf.existsAsFile()) logoImg = juce::ImageFileFormat::loadFrom (lf); }
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
        addChildComponent (settingsPanel);

        audioCfg.onDevice = [this] (const juce::String& d) { applyAudioDevice (d); };
        audioCfg.onRoute  = [this] (int f, int m, int b)   { setFamRoute (f, m, b); };
        addChildComponent (audioCfg);

        liveServer.getPage  = [] { return juce::String (juce::CharPointer_UTF8 (kMusicianPage)); };
        liveServer.getSong  = [this] { const juce::ScopedLock l (chartLock); return currentChartJson; };
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
        addChildComponent (repEdit);
        padBtn.setButtonText ("PAD");
        padBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        padBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        addAndMakeVisible (padBtn);

        connStatus.setJustificationType (juce::Justification::centred);
        connStatus.setColour (juce::Label::textColourId, juce::Colour (0xffa3a3a3));
        connStatus.setFont (juce::Font (12.0f));
        addAndMakeVisible (connStatus);

        playButton.setButtonText ("Play");
        playButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffffffff));
        playButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff0a0a0a));
        playButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible (playButton);

        returnButton.setButtonText ("|" + juce::String::charToString ((juce_wchar) 0x25C0));
        returnButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        returnButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        returnButton.onClick = [this] { seekSeconds (0.0); };
        addAndMakeVisible (returnButton);

        fadeButton.setButtonText ("Fade");
        fadeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        fadeButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffffffff));
        fadeButton.onClick = [this] { toggleFade(); };
        addAndMakeVisible (fadeButton);

        timeLabel.setJustificationType (juce::Justification::centred);
        timeLabel.setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
        timeLabel.setFont (juce::Font (14.0f, juce::Font::bold));
        addAndMakeVisible (timeLabel);

        masterSlider.setSliderStyle (juce::Slider::LinearVertical);
        masterSlider.setRange (-60.0, 6.0, 0.1);
        masterSlider.setValue (0.0);
        masterSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        masterSlider.textFromValueFunction = [] (double v) { return dbText (v); };
        masterSlider.valueFromTextFunction = [] (const juce::String& t) { return t.containsIgnoreCase ("inf") ? -60.0 : t.getDoubleValue(); };
        masterSlider.onValueChange = [this]
        {
            masterGain.store (dbToGain ((float) masterSlider.getValue()));
            if (currentSong >= 0 && currentSong < songMaster.size())
                songMaster.set (currentSong, masterSlider.getValue());
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
        faderViewBtn.kind = 0; repeatBtn.kind = 1; infiniteBtn.kind = 2;
        faderViewBtn.active = true;
        addAndMakeVisible (faderViewBtn);
        addAndMakeVisible (repeatBtn);
        addAndMakeVisible (infiniteBtn);
        faderViewBtn.onClick = [this] { setFaderView (0); };
        busesBtn.onClick     = [this] { setFaderView (1); };
        muteMidiBtn.onClick  = [this] { setFaderView (2); };

        repeatBtn.onClick = [this]
        {
            if (loopOnce.load())   // ya estaba armado -> cancelar
            {
                loopOnce.store (false);
                if (! loopActive.load()) { loopStartSec.store (-1.0); loopEndSec.store (-1.0); }
            }
            else
            {
                double t0, t1; currentSectionRange (positionSeconds(), t0, t1);
                loopStartSec.store (t0);
                loopEndSec.store (t1);
                loopOnce.store (true);
            }
        };
        infiniteBtn.onClick = [this]
        {
            const bool on = ! loopActive.load();
            if (on)
            {
                double t0, t1; currentSectionRange (positionSeconds(), t0, t1);
                loopStartSec.store (t0);
                loopEndSec.store (t1);
                loopActive.store (true);
            }
            else
            {
                loopActive.store (false);
                loopOnce.store (false);
                loopStartSec.store (-1.0);
                loopEndSec.store (-1.0);
            }
            infiniteBtn.active = on;
            infiniteBtn.repaint();
        };

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
        rebuildMidiOuts();
        midiClock.tick = [this] { fireMidiRT(); };
        midiClock.startTimer (1);   // ~1 ms: minima latencia MIDI, hilo dedicado

        setSize (1040, 906);
        setAudioChannels (0, 12);   // hasta 12 salidas (interfaces multicanal)
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.bufferSize = 256;   // menor latencia de salida (sin comprometer estabilidad)
        setup.sampleRate = 44100.0;
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
                openOutChans = juce::jlimit (1, 12, currentOutputChannelCount());
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
        setLookAndFeel (nullptr);
        thumb.removeChangeListener (this);
        shutdownAudio();
    }

    void prepareToPlay (int spb, double sampleRate) override
    {
        const juce::ScopedLock sl (graphLock);
        deviceSampleRate = sampleRate;
        currentBlockSize = spb;
        temp.setSize (2, spb + 8);
        for (int i = 0; i < resamplers.size(); ++i)
        {
            resamplers[i]->setResamplingRatio (fileRates[i] / sampleRate);
            resamplers[i]->prepareToPlay (spb, sampleRate);
        }
        prepared.store (true);
    }
    void releaseResources() override
    {
        prepared.store (false);
        for (auto* r : resamplers) r->releaseResources();
    }
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
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
        const int useCh = juce::jmin (nch, 12);
        float* out[12] = { nullptr };
        for (int c = 0; c < useCh; ++c) out[c] = info.buffer->getWritePointer (c, info.startSample);

        bool anySolo = false;
        for (int t = 0; t < resamplers.size(); ++t) if (trackSolo[t].load()) { anySolo = true; break; }

        for (int t = 0; t < resamplers.size(); ++t)
        {
            temp.clear();
            juce::AudioSourceChannelInfo ti (&temp, 0, nn);
            resamplers.getUnchecked (t)->getNextAudioBlock (ti);
            const bool audible = ! trackMuted[t].load() && (! anySolo || trackSolo[t].load());
            const float g = audible ? trackGain[t].load() * busGain[trackFamily[t]].load() : 0.0f;
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
        const float m = masterGain.load();
        for (int c = 0; c < useCh; ++c) { float* o = out[c]; for (int n = 0; n < nn; ++n) o[n] = softClip (o[n] * m); }
        positionOut.fetch_add (nn);

        const double posSec = (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate);
        if ((loopActive.load() || loopOnce.load()) && loopEndSec.load() > 0.0 && posSec >= loopEndSec.load())
        {
            const double fr2 = fileRates.isEmpty() ? 44100.0 : fileRates[0];
            seekTo.store ((long long) (juce::jmax (0.0, loopStartSec.load()) * fr2));
            if (loopOnce.load())
            {
                loopOnce.store (false);
                if (! loopActive.load()) { loopStartSec.store (-1.0); loopEndSec.store (-1.0); }
            }
        }
        else
        {
            const long long totalDev = (long long) (totalSeconds() * deviceSampleRate);
            if (totalDev > 0 && positionOut.load() >= totalDev) { playing.store (false); seekTo.store (0); }   // fin: parar y volver al inicio
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0a0a));
        drawLogo (g, 28.0f, 16.0f);

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
            g.setColour (juce::Colour (0xff5a6577));
            g.setFont (12.0f);
            g.drawText (currentSong < 0 ? "Conecta para traer el repertorio" : "Cargando forma de onda...",
                        inner, juce::Justification::centred);
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

            if (masterSepX > 0)
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
        const double dx = e.getDistanceFromDragStartX() * (win / juce::jmax (1, inner.getWidth()));
        lastInteractionMs = juce::Time::getMillisecondCounter();
        if (dragSeeks) seekSeconds (dragStartCenter - dx);
        else { browsing = true; browseCenter = juce::jlimit (0.0, totalSeconds(), dragStartCenter - dx); repaint (mapBounds); }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isDragging) return;
        isDragging = false;
        if (dragSeeks) { if (e.getDistanceFromDragStart() < 5) seekFromMouse (e); }
        else { browsing = false; repaint (mapBounds); }
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (! mapBounds.contains (e.getPosition())) return;
        double d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        if (w.isReversed) d = -d;
        if (playing.load())
        {
            if (! browsing) browseCenter = positionSeconds();
            browsing = true;
            browseCenter = juce::jlimit (0.0, totalSeconds(), browseCenter + d * 60.0);
            lastInteractionMs = juce::Time::getMillisecondCounter();
            repaint (mapBounds);
        }
        else seekSeconds (positionSeconds() + d * 60.0);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16);

        // Barra superior: logo (izq) | Play + Inicio (centro) | Conectar + tiempo (der)
        auto topbar = area.removeFromTop (46);
        {
            const int BW = 80, BH = 34, G = 8;
            const int gy = topbar.getCentreY() - BH / 2;

            // Izquierda (tras el logo): caja de tiempo + PAD
            const int logoW = 150;
            int lx = topbar.getX() + logoW;
            timeLabel.setBounds (lx, gy, 106, BH);
            lx += 106 + G;
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

        // Tarjetas verticales grandes: portada arriba + nombre abajo
        auto strip = area.removeFromTop (180);
        {
            int x = strip.getX();
            for (auto* c : songCards) { c->setBounds (x, strip.getY(), 240, 178); x += 250; }
            if (editMode) addCard.setBounds (x, strip.getY(), 240, 178);
        }
        area.removeFromTop (4);

        mapBounds = area.removeFromTop (188);
        area.removeFromTop (8);
        faderPanelBounds = area;

        // Region FIJA a la derecha: separador doble + 6 botones + Master (no se desplazan)
        const int rightW = 20 + 62 + 10 + 78 + 8;
        auto fixed = area.removeFromRight (rightW);

        // Los tracks (solo esos) van en el viewport desplazable
        faderViewport.setBounds (area);
        midiPanel.setBounds (area);
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
            const float hh = 40.0f;
            const float ww = hh * (float) logoImg.getWidth() / (float) juce::jmax (1, logoImg.getHeight());
            g.drawImage (logoImg, juce::Rectangle<float> (x, y - 4.0f, ww, hh), juce::RectanglePlacement::centred);
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

    void applyAudioDevice (const juce::String& name)
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName = name;
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        setup.outputChannels.setRange (0, 12, true);
        setup.bufferSize = 256;
        setup.sampleRate = 44100.0;
        deviceManager.setAudioDeviceSetup (setup, true);
        audioOutDevice = name;
        openOutChans = juce::jlimit (1, 12, currentOutputChannelCount());
        ensureDeviceRoutes (name);
        snapshotRoutes();
        audioCfg.buildRouteItems (openOutChans);
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
            openOutChans = juce::jlimit (1, 12, currentOutputChannelCount());
            snapshotRoutes();
        }
        audioCfg.setBounds (getLocalBounds());
        audioCfg.setDevices (outputDeviceNames(), audioOutDevice);
        audioCfg.buildRouteItems (openOutChans);
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
    void toggleEdit()
    {
        editMode = ! editMode;
        editBtn.setColour (juce::TextButton::buttonColourId, editMode ? juce::Colour (0xff2E6BE6) : juce::Colour (0xff1f1f1f));
        editBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        editBtn.repaint();
        rebuildRepertoireStrip();
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
        juce::StringPairArray p; p.set ("numero", juce::String (songId)); p.set ("tono", tonoName);
        postThenReload (serverUrl + "/api/live/setlist/" + lastSetlistId + "/agregar", p);
    }

    void createSetlist()
    {
        auto* aw = new juce::AlertWindow (juce::String::fromUTF8 ("Nuevo repertorio"),
                                          juce::String::fromUTF8 ("Nombre del repertorio:"),
                                          juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("n", "");
        aw->addButton ("Crear", 1);
        aw->addButton ("Cancelar", 0);
        juce::Component::SafePointer<MainComponent> sp (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([sp, aw] (int r)
        {
            const juce::String nombre = aw->getTextEditorContents ("n").trim();
            if (r == 1 && sp != nullptr)
                sp->doCreateSetlist (nombre.isNotEmpty() ? nombre : juce::String ("Nuevo repertorio"));
        }), true);
    }

    void doCreateSetlist (juce::String nombre)
    {
        if (serverUrl.isEmpty()) return;
        const auto base = serverUrl + "/api/live/setlist/crear"; const auto tok = serverToken;
        juce::StringPairArray p; p.set ("nombre", nombre);
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
                sp->repEdit.openTono (songId, title, addFlow, ks);
                sp->repEdit.setVisible (true); sp->repEdit.toFront (true);
            });
        });
    }

    void startLoadId (juce::String setlistId)
    {
        if (serverToken.isEmpty()) { connStatus.setText ("Falta servidor/token", juce::dontSendNotification); return; }
        if (loader && loader->isThreadRunning()) return;
        lastSetlistId = setlistId;
        connStatus.setText ("Actualizando...", juce::dontSendNotification);
        loader = std::make_unique<RepertoireLoader> (serverUrl, serverToken, npAppDir().getChildFile ("cache"));
        loader->wantedId = setlistId;
        juce::Component::SafePointer<MainComponent> sp (this);
        loader->onStatus = [sp] (juce::String s) { if (sp) sp->connStatus.setText (s, juce::dontSendNotification); };
        loader->onDone   = [sp] (juce::Array<SongEntry> songs) { if (sp) sp->onRepertoireLoaded (songs); };
        loader->startThread();
    }

    void openRepertoirePicker()
    {
        repPicker.loading = true;
        repPicker.items.clearQuick();
        repPicker.selected = -1;
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
                    if (auto* cs = s.getProperty ("canciones", juce::var()).getArray()) it.nCanciones = cs->size();
                    its.add (it);
                }
            juce::MessageManager::callAsync ([sp, its]
            {
                if (sp != nullptr && sp->repPicker.isVisible()) sp->repPicker.setItems (its);
            });
        });
    }

    void onRepertoireLoaded (juce::Array<SongEntry> songs)
    {
        if (loader && loader->resolvedId.isNotEmpty()) lastSetlistId = loader->resolvedId;   // id real del setlist
        repertoire = songs;
        songMaster.clearQuick();
        for (int i = 0; i < repertoire.size(); ++i) songMaster.add (0.0);
        for (auto& e : repertoire)
            if (e.coverFile.existsAsFile()) e.cover = juce::ImageFileFormat::loadFrom (e.coverFile);
        rebuildRepertoireStrip();
        if (! repertoire.isEmpty()) loadSong (0);
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
            const int idx = i; const int sid = s.id; const juce::String title = s.titulo;
            c->onClick  = [this, idx] { loadSong (idx); };
            c->onRemove = [this, sid] { removeSong (sid); };
            c->onTono   = [this, sid, title] { openTonoFor (sid, title, false); };
            addAndMakeVisible (c);
        }
        addCard.setVisible (editMode);
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
        currentSong = index;
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
        playButton.setButtonText ("Play");

        thumb.clear();
        if (! stemFiles.isEmpty())
        {
            mixBuilder = std::make_unique<MixThumb> (thumb, formatManager, stemFiles,
                                                     fileRates.isEmpty() ? 44100.0 : fileRates[0], lengthSamples);
            mixBuilder->startThread();
        }

        {
            const double mv = (index >= 0 && index < songMaster.size()) ? songMaster[index] : 0.0;
            masterSlider.setValue (mv, juce::dontSendNotification);
            masterGain.store (dbToGain ((float) mv));
        }
        loopActive.store (false); loopOnce.store (false);
        loopStartSec.store (-1.0); loopEndSec.store (-1.0);
        infiniteBtn.active = false; infiniteBtn.repaint();
        waveImg = juce::Image(); waveDirty = true;

        {
            const juce::ScopedLock sl (midiLock);
            currentMidiBoxes = sng.midiBoxes;
            flushMidiOffs();
            recalcMidiNext (0.0);
            midiCursor = 0.0;
        }

        liveSectionIdx.store (0);
        if (syncEnabled) fetchLiveChart (sng.id, sng.tono);

        rebuildMixerUI();
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
            s->onValueChange = [this, idx] { trackGain[idx].store (dbToGain ((float) trackSliders[idx]->getValue())); };
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
                const bool m = ! trackMuted[idx].load();
                trackMuted[idx].store (m);
                trackLabels[idx]->setColour (juce::Label::textColourId,
                    m ? juce::Colour (0xffe05555) : juce::Colour (0xfff2f2f2));
                trackLabels[idx]->repaint();
                trackSliders[idx]->getProperties().set ("muted", m);
                trackSliders[idx]->repaint();
            };
            faderStrip.addAndMakeVisible (l);

            auto* d = soloDots.add (new SoloDot());
            d->onClick = [this, idx]
            {
                const bool on = ! trackSolo[idx].load();
                trackSolo[idx].store (on);
                soloDots[idx]->on = on;
                soloDots[idx]->repaint();
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
        }
        for (int f = 0; f < familyNames.size() && f < 16; ++f)
        {
            busGain[f].store (1.0f);
            auto* s = busSliders.add (new juce::Slider());
            s->setSliderStyle (juce::Slider::LinearVertical);
            s->setRange (-60.0, 6.0, 0.1);
            s->setValue (0.0);
            s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s->textFromValueFunction = [] (double v) { return dbText (v); };
            const int bf = f;
            s->onValueChange = [this, bf] { busGain[bf].store (dbToGain ((float) busSliders[bf]->getValue())); };
            s->setLookAndFeel (&faderLnf);
            s->setRepaintsOnMouseActivity (false);
            faderStrip.addAndMakeVisible (s);

            auto* l = busLabels.add (new ClickLabel());
            l->setText (familyNames[f], juce::dontSendNotification);
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
            l->setFont (juce::Font (12.0f, juce::Font::bold));
            l->onClick = [this, bf] { toggleBusMute (bf); };
            faderStrip.addAndMakeVisible (l);

            auto* d = busSoloDots.add (new SoloDot());
            d->onClick = [this, bf] { toggleBusSolo (bf); };
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
        faderViewport.setVisible (! midi);
        midiPanel.setVisible (midi);
        for (auto* s : trackSliders) s->setVisible (tracks);
        for (auto* l : trackLabels)  l->setVisible (tracks);
        for (auto* d : soloDots)     d->setVisible (tracks);
        for (auto* s : busSliders)   s->setVisible (buses);
        for (auto* l : busLabels)    l->setVisible (buses);
        for (auto* d : busSoloDots)  d->setVisible (buses);
    }

    void setFaderView (int v)
    {
        faderView = juce::jlimit (0, 2, v);
        faderViewBtn.active   = (faderView == 0);
        busesBtn.setColour    (juce::TextButton::buttonColourId, faderView == 1 ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff1f1f1f));
        muteMidiBtn.setColour (juce::TextButton::buttonColourId, faderView == 2 ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff1f1f1f));
        faderViewBtn.repaint(); busesBtn.repaint(); muteMidiBtn.repaint();
        if (faderView == 1) refreshBusStates();
        if (faderView == 2) midiPanel.refreshPorts();
        updateFaderVisibility();
        if (faderView != 2) layoutFaderStrip();
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
        const double t = totalSeconds();
        return t > 0 ? juce::jlimit (0.0, t, (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate)) : 0.0;
    }
    void timerCallback() override
    {
        if (splashOn && juce::Time::getMillisecondCounter() - splashStart > 1600) { splashOn = false; splash.setVisible (false); }

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
        timeLabel.setText (fmtTime (positionSeconds()) + " / " + fmtTime (totalSeconds()), juce::dontSendNotification);
        if (browsing && ! isDragging && (juce::Time::getMillisecondCounter() - lastInteractionMs > 1200))
        { browsing = false; repaint (mapBounds); }
        if (playing.load() || browsing) repaint (mapBounds);

        if (fadeDir != 0)
        {
            bool done = true;
            const double step = 0.33;  // dB por tick (~3s de desvanecimiento)
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

        if (repeatBtn.active   != loopOnce.load())   { repeatBtn.active   = loopOnce.load();   repeatBtn.repaint(); }
        if (infiniteBtn.active != loopActive.load()) { infiniteBtn.active = loopActive.load(); infiniteBtn.repaint(); }
    }
    void togglePlay()
    {
        if (resamplers.isEmpty()) return;
        const bool p = ! playing.load();
        if (p && positionSeconds() >= totalSeconds() - 0.1) seekSeconds (0.0);
        playing.store (p);
        playButton.setButtonText (p ? "Pausa" : "Play");
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
    void seekFromMouse (const juce::MouseEvent& e)
    {
        auto inner = mapBounds.reduced (8);
        if (! inner.contains (e.getPosition())) return;
        double vs = 0.0, ve = 0.0; getViewWindow (vs, ve);
        const double frac = juce::jlimit (0.0, 1.0, (double) (e.x - inner.getX()) / juce::jmax (1, inner.getWidth()));
        seekSeconds (vs + frac * (ve - vs));
    }
    void seekSeconds (double sec)
    {
        const double fr = fileRates.isEmpty() ? 44100.0 : fileRates[0];
        seekTo.store ((long long) (juce::jmax (0.0, sec) * fr));
        repaint (mapBounds);
    }

    juce::String serverUrl, serverToken;
    juce::Array<SongEntry> repertoire;
    juce::Array<double> songMaster;   // master (dB) independiente por cancion
    int currentSong = -1;
    std::unique_ptr<RepertoireLoader> loader;

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
    int beatsPerBar = 4;
    juce::AudioBuffer<float> temp;
    juce::Rectangle<int> mapBounds;
    juce::Rectangle<int> faderPanelBounds;
    bool splashOn = true;
    juce::uint32 splashStart = 0;
    int numTracks = 0;
    long long lengthSamples = 0;
    double deviceSampleRate = 44100.0;
    int currentBlockSize = 0;
    juce::CriticalSection graphLock;

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
    std::atomic<float> masterGain { 1.0f };

    std::atomic<float> busGain[16];         // ganancia por familia (bus)
    int trackFamily[kMaxTracks] = { 0 };    // familia (bus) de cada track
    int faderView = 0;                      // 0 = tracks, 1 = buses
    int vuTick = 0;                         // para refrescar el VU a la mitad de FPS

    std::atomic<bool> loopActive { false };     // infinito (permanente)
    std::atomic<bool> loopOnce { false };       // repetir una vez
    std::atomic<double> loopStartSec { -1.0 };
    std::atomic<double> loopEndSec { -1.0 };

    juce::Image logoImg;
    PillLNF pillLnf;
    FaderLNF faderLnf;
    juce::TextButton connectButton, playButton, returnButton, fadeButton;
    juce::Array<double> preFadeVals;
    int fadeDir = 0;            // -1 bajando, +1 subiendo, 0 quieto
    bool fadedDown = false;
    juce::Label connStatus, timeLabel, masterLabel;
    juce::Slider masterSlider;
    juce::TextButton busesBtn, padPlayerBtn, muteMidiBtn, editBtn, padBtn;
    IconButton faderViewBtn, repeatBtn, infiniteBtn, settingsBtn, repertoireBtn;
    FaderStripComp faderStrip;
    juce::Viewport faderViewport;
    MidiPanel midiPanel;
    RepertoirePicker repPicker;
    SettingsPanel settingsPanel;
    juce::String lastSetlistId;   // setlist cargado (para "Actualizar")
    RepEditPanel repEdit;
    AddCard addCard;
    bool editMode = false;
    juce::Array<RepEditPanel::BibItem> bibliotecaAll;
    bool syncEnabled = false;
    std::atomic<bool> syncLinked { false };
    int syncPingCtr = 0, syncPollCtr = 0;
    HttpLiveServer liveServer;
    juce::String currentChartJson { "{}" };
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
    juce::OwnedArray<juce::MidiOutput> midiOuts;
    juce::Array<juce::MidiOutput*> cajaOut;
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

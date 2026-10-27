#pragma once
// ─────────────────────────────────────────────────────────────
// np_ui.h — Componentes visuales de NeuralPlay (LookAndFeel, botones,
//   tarjetas, paneles, overlay de login, servidor En Vivo). Sin lógica de
//   audio ni la ventana principal. Extraído de Main.cpp sin cambios.
// ─────────────────────────────────────────────────────────────
#include <JuceHeader.h>
#include "BinaryData.h"
#include "np_util.h"
#include "np_net.h"
#include "np_keychain.h"
#include <map>
#include <array>
#include <functional>

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
        const float top = (float) y + 2.0f;
        const float bot = (float) y + height - 2.0f;

        // Barra vertical con redondeo leve: riel + relleno.
        const float barW = juce::jmin ((float) width - 6.0f, 35.0f);
        const float rad = 3.0f;
        juce::Rectangle<float> track (cx - barW * 0.5f, top, barW, bot - top);
        g.setColour (juce::Colour (0xff17181c));
        g.fillRoundedRectangle (track, rad);
        g.setColour (juce::Colour (0xff2a2b30));
        g.drawRoundedRectangle (track.reduced (0.5f), rad, 1.0f);

        // Escala tenue de dB (−60..0, lineal) a los lados del riel: look de mezcladora.
        {
            const int marks[] = { 0, -6, -12, -24, -48 };
            for (int db : marks)
            {
                const float fy = top + (float) (-db) / 60.0f * (bot - top);
                g.setColour (juce::Colour (0x3affffff));
                g.fillRect (track.getX() - 4.0f, fy - 0.5f, 3.0f, 1.0f);          // tick izquierda
                g.fillRect (track.getRight() + 1.0f, fy - 0.5f, 3.0f, 1.0f);      // tick derecha
                g.setColour (juce::Colour (0x66ffffff));
                g.setFont (juce::Font (7.0f));
                g.drawText (db == 0 ? juce::String ("0") : juce::String (db),
                            track.getRight() + 4.5f, fy - 4.5f, 13.0f, 9.0f,
                            juce::Justification::centredLeft, false);
            }
        }

        const bool muted = (bool) slider.getProperties().getWithDefault ("muted", false);
        const bool dim   = (bool) slider.getProperties().getWithDefault ("dim", false);   // opacar al haber solo en otro canal
        juce::Colour fillCol = muted ? juce::Colour (0xffE0433E) : juce::Colour (0xffF2F2F2);
        if (dim) fillCol = fillCol.withMultipliedAlpha (0.28f);

        // Relleno de volumen: desde abajo hasta la posición actual del fader.
        const float fillTop = juce::jlimit (top, bot, sliderPos);
        if (bot - fillTop > 1.0f)
        {
            juce::Rectangle<float> fill (cx - barW * 0.5f, fillTop, barW, bot - fillTop);
            juce::ColourGradient fg (fillCol.brighter (0.10f), cx, fillTop,
                                     fillCol.darker (0.18f), cx, bot, false);
            g.setGradientFill (fg);
            g.fillRoundedRectangle (fill, rad);
            // tope del nivel: línea oscura para que se note sobre la barra clara.
            g.setColour (juce::Colour (dim ? 0x22000000 : 0x66000000));
            g.fillRoundedRectangle (cx - barW * 0.5f, fillTop - 1.5f, barW, 3.0f, 1.0f);
        }
    }
};

struct PillLNF : public juce::LookAndFeel_V4
{
    PillLNF()
    {
        setDefaultSansSerifTypefaceName ("Helvetica Neue");
        setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
        // Cajas de selección con el mismo look oscuro y fino que los campos.
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff202227));
        setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff3a3d44));
        setColour (juce::ComboBox::textColourId,       juce::Colour (0xfff2f2f2));
        setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xffbfc4cc));
        // Menú desplegable (popup) fino y oscuro para TODOS los ComboBox de la app.
        setColour (juce::PopupMenu::backgroundColourId,            juce::Colour (0xff1c1e22));
        setColour (juce::PopupMenu::textColourId,                  juce::Colour (0xffe8e8e8));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff2b2f36));
        setColour (juce::PopupMenu::highlightedTextColourId,       juce::Colours::white);
    }
    // ── Popup de los ComboBox: filas compactas, fondo redondeado oscuro ──
    juce::Font getPopupMenuFont() override { return juce::Font (14.0f); }
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                    int stdHeight, int& idealW, int& idealH) override
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, stdHeight, idealW, idealH);
        if (! isSeparator) idealH = npEsIPhone() ? 34 : 30;
    }
    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.setColour (juce::Colour (0xff1c1e22));
        g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, 10.0f);
        g.setColour (juce::Colour (0x22ffffff));
        g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, 10.0f, 1.0f);
    }
    // Fuente del CUERPO de la app: Inter (empaquetada), legible en tamaños chicos.
    // El login usa Space Grotesk (marca) con su propia fuente explícita; los pads
    // usan "Futura" con nombre propio. Ambos se respetan tal cual.
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& f) override
    {
        const auto name = f.getTypefaceName();
        const bool esDefault = name.isEmpty()
                            || name == juce::Font::getDefaultSansSerifFontName()
                            || name == "Helvetica Neue";
        if (esDefault)
        {
            static juce::Typeface::Ptr reg = juce::Typeface::createSystemTypefaceFor (
                BinaryData::InterRegular_ttf, (size_t) BinaryData::InterRegular_ttfSize);
            static juce::Typeface::Ptr bld = juce::Typeface::createSystemTypefaceFor (
                BinaryData::InterBold_ttf,    (size_t) BinaryData::InterBold_ttfSize);
            return f.isBold() ? bld : reg;
        }
        return juce::LookAndFeel_V4::getTypefaceForFont (f);
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
    // Cajas de selección (ComboBox) con esquinas redondeadas, igual que los campos.
    void drawComboBox (juce::Graphics& g, int width, int height, bool,
                       int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.7f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (r, 10.0f, 1.0f);
        juce::Path p;                                   // flecha ▾
        const float cx = (float) width - 18.0f, cy = (float) height * 0.5f;
        p.startNewSubPath (cx - 5.0f, cy - 2.5f);
        p.lineTo (cx,        cy + 3.0f);
        p.lineTo (cx + 5.0f, cy - 2.5f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (0.9f));
        g.strokePath (p, juce::PathStrokeType (1.6f));
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
    bool flipped = false;   // true = fade activo (bajando): el icono se voltea
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
        if (flipped) ramp.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f, c.x, c.y));   // voltear horizontal al activarse
        g.setColour (isEnabled() ? juce::Colours::white.withAlpha (0.92f) : juce::Colours::white.withAlpha (0.4f));
        g.fillPath (ramp);
    }
};

// Boton "ir al inicio" (barra vertical + triangulo a la izquierda), dibujado como
// vector para verse IDENTICO en Mac y iPad (los glifos de texto los sustituye iOS).
struct SkipStartButton : public juce::Button
{
    SkipStartButton() : juce::Button ("inicio") {}
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float rad = juce::jmin (r.getHeight() * 0.5f, 12.0f);
        juce::Colour bg (0xff1f1f1f);
        if (down) bg = bg.brighter (0.06f); else if (over) bg = bg.brighter (0.10f);
        g.setColour (bg); g.fillRoundedRectangle (r, rad);
        g.setColour (juce::Colour (0xff2a2a2a)); g.drawRoundedRectangle (r, rad, 1.0f);

        const auto c = r.getCentre();
        const float s = juce::jmin (r.getHeight(), r.getWidth());
        const float tw = s * 0.26f, th = s * 0.40f;
        const float barW = s * 0.085f, gap = s * 0.06f;
        const float groupW = barW + gap + tw;
        const float leftX = c.x - groupW * 0.5f;
        g.setColour (juce::Colours::white.withAlpha (isEnabled() ? 0.95f : 0.4f));
        g.fillRoundedRectangle (leftX, c.y - th * 0.5f, barW, th, barW * 0.4f);   // barra "|"
        juce::Path tri;                                                            // triangulo a la izquierda
        const float triX = leftX + barW + gap;
        tri.addTriangle (triX + tw, c.y - th * 0.5f,
                         triX + tw, c.y + th * 0.5f,
                         triX,      c.y);
        g.fillPath (tri);
    }
};

// Boton "siguiente / transicion" (triangulo a la derecha + barra), dibujado como
// vector para verse IDENTICO en Mac y iPad/iPhone (iOS sustituye los glifos de texto).
struct SkipNextButton : public juce::Button
{
    SkipNextButton() : juce::Button ("siguiente") {}
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float rad = juce::jmin (r.getHeight() * 0.5f, 12.0f);
        juce::Colour bg (0xff2E6BE6);   // azul (igual que Play / transporte)
        if (down) bg = bg.brighter (0.06f); else if (over) bg = bg.brighter (0.10f);
        g.setColour (bg); g.fillRoundedRectangle (r, rad);
        g.setColour (juce::Colour (0x332E6BE6)); g.drawRoundedRectangle (r, rad, 1.0f);

        const auto c = r.getCentre();
        const float s = juce::jmin (r.getHeight(), r.getWidth());
        const float tw = s * 0.26f, th = s * 0.40f;
        const float barW = s * 0.085f, gap = s * 0.06f;
        const float groupW = tw + gap + barW;
        const float leftX = c.x - groupW * 0.5f;
        g.setColour (juce::Colours::white.withAlpha (isEnabled() ? 0.95f : 0.4f));
        juce::Path tri;                                                  // triangulo apuntando a la derecha
        tri.addTriangle (leftX,      c.y - th * 0.5f,
                         leftX,      c.y + th * 0.5f,
                         leftX + tw, c.y);
        g.fillPath (tri);
        const float barX = leftX + tw + gap;                            // barra "|" a la derecha
        g.fillRoundedRectangle (barX, c.y - th * 0.5f, barW, th, barW * 0.4f);
    }
};

// Boton de flecha triangular (navegacion por seccion del mapa), vector cross-platform.
struct TriIconButton : public juce::Button
{
    bool pointsRight = false;
    TriIconButton() : juce::Button ("tri") {}
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float rad = juce::jmin (r.getHeight() * 0.5f, 10.0f);
        juce::Colour bg (0xcc1a1a1a);
        if (down) bg = bg.brighter (0.06f); else if (over) bg = bg.brighter (0.10f);
        g.setColour (bg); g.fillRoundedRectangle (r, rad);

        const auto c = r.getCentre();
        const float s = juce::jmin (r.getHeight(), r.getWidth());
        const float w = s * 0.30f, h = s * 0.42f;
        g.setColour (juce::Colours::white.withAlpha (isEnabled() ? 0.95f : 0.4f));
        juce::Path tri;
        if (pointsRight)
            tri.addTriangle (c.x - w * 0.5f, c.y - h * 0.5f,
                             c.x - w * 0.5f, c.y + h * 0.5f,
                             c.x + w * 0.5f, c.y);
        else
            tri.addTriangle (c.x + w * 0.5f, c.y - h * 0.5f,
                             c.x + w * 0.5f, c.y + h * 0.5f,
                             c.x - w * 0.5f, c.y);
        g.fillPath (tri);
    }
};

struct SongCard : public juce::Component
{
    juce::Image cover;
    juce::String titulo, tono;
    juce::String arrNombre;   // arreglo activo (vacío = Original): se muestra como chip en la portada
    bool active = false;
    bool pending = false;   // (en desuso) encolada
    bool ready = true;      // false = aún descargando / en cola: se ve atenuada y NO se puede seleccionar
    bool editMode = false;
    int index = 0;
    int songId = 0;   // #4 id de la canción (para mapping de teclado)
    std::function<void()> onClick, onRemove, onTono, onAddAfter;
    std::function<void (int fromIndex, int toIndex)> onReorder;   // arrastrar para reordenar
    float dlProgress = -1.0f;   // -1 = sin barra; 0..1 = progreso
    int   dlKind = 2;           // 1 = Transponiendo (render en servidor), 2 = Descargando

    int homeX = 0, homeY = 0; bool dragging = false;

    // Versión aclarada de la portada (realce de brillo para que las carátulas oscuras se lean bien).
    juce::Image coverBrightCache;
    juce::ImagePixelData* coverBrightSrc = nullptr;
    const juce::Image& brightCover()
    {
        if (! cover.isValid()) return cover;
        if (coverBrightCache.isValid() && coverBrightSrc == cover.getPixelData().get())
            return coverBrightCache;
        // En macOS la copia nativa (CoreGraphics) no deja escribir píxeles con BitmapData:
        // forzamos un respaldo SOFTWARE para que el realce de brillo sí se aplique.
        coverBrightCache = juce::SoftwareImageType().convert (cover);
        juce::Image::BitmapData bd (coverBrightCache, juce::Image::BitmapData::readWrite);
        auto lift = [] (juce::uint8 v) { return (juce::uint8) juce::jlimit (0, 255, (int) std::lround (28.0 + 255.0 * std::pow (v / 255.0, 0.5))); };
        for (int y = 0; y < bd.height; ++y)
            for (int x = 0; x < bd.width; ++x)
            {
                auto c = bd.getPixelColour (x, y);
                bd.setPixelColour (x, y, juce::Colour (lift (c.getRed()), lift (c.getGreen()), lift (c.getBlue())));
            }
        coverBrightSrc = cover.getPixelData().get();
        return coverBrightCache;
    }

    // ── Fila de lista: miniatura cuadrada a la izquierda, título a la derecha, botones de edición a la derecha ──
    float rowBtn() const { return npEsIPhone() ? 20.0f : 23.0f; }
    juce::Rectangle<float> coverRect() const   // miniatura cuadrada (izquierda)
    {
        auto r = getLocalBounds().toFloat().reduced (0.0f, 4.0f);
        const float s = juce::jmax (10.0f, r.getHeight());
        return { r.getX() + 7.0f, r.getY(), s, s };
    }
    // Botones de edición alineados a la derecha de la fila: [⋯ tono] [− quitar] [+ agregar].
    juce::Rectangle<float> addBtnRect() const
    {
        auto r = getLocalBounds().toFloat();
        const float bs = rowBtn(), m = npEsIPhone() ? 8.0f : 12.0f;
        return { r.getRight() - bs - m, r.getCentreY() - bs * 0.5f, bs, bs };
    }
    juce::Rectangle<float> removeBtnRect() const { auto a = addBtnRect(); const float gp = npEsIPhone() ? 8.0f : 10.0f; return a.translated (-(a.getWidth() + gp), 0.0f); }
    juce::Rectangle<float> tonoBtnRect()   const { auto a = addBtnRect(); const float gp = npEsIPhone() ? 8.0f : 10.0f; return a.translated (-2.0f * (a.getWidth() + gp), 0.0f); }

    void mouseDown (const juce::MouseEvent&) override
    {
        homeX = getX(); homeY = getY(); dragging = false;
    }
    // Resaltado al pasar el mouse por los botones de edición.
    void mouseMove  (const juce::MouseEvent&) override { if (editMode) repaint(); }
    void mouseEnter (const juce::MouseEvent&) override { if (editMode) repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { if (editMode) repaint(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! editMode) return;
        const int dy = e.getDistanceFromDragStartY();   // reordenar VERTICAL (lista)
        if (dragging || std::abs (dy) > 5)
        {
            dragging = true;
            toFront (false);
            setTopLeftPosition (getX(), homeY + dy);
        }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (editMode && dragging)
        {
            dragging = false;
            const int step  = juce::jmax (1, getHeight() + 6);
            const int steps = juce::roundToInt ((float) e.getDistanceFromDragStartY() / (float) step);
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
        auto full = getLocalBounds().toFloat();
        auto rowBg = full.reduced (2.0f, 2.0f);
        // Mismo fondo para todas (no se oscurecen las no seleccionadas); marco BLANCO en la activa.
        g.setColour (juce::Colour (0x12ffffff));
        g.fillRoundedRectangle (rowBg, 8.0f);
        if (active)       { g.setColour (juce::Colours::white);          g.drawRoundedRectangle (rowBg, 8.0f, 2.0f); }
        else if (pending) { g.setColour (juce::Colour (0xffE0A43A));     g.drawRoundedRectangle (rowBg, 8.0f, 1.6f); }  // ámbar: encolada/descargando
        else              { g.setColour (juce::Colour (0x1affffff));     g.drawRoundedRectangle (rowBg, 8.0f, 1.0f); }

        // Miniatura (portada chica) a la izquierda.
        auto cov = coverRect();
        {
            juce::Path clip; clip.addRoundedRectangle (cov, 6.0f);
            g.saveState(); g.reduceClipRegion (clip);
            g.setOpacity (1.0f);   // IMPORTANTE: drawImage usa la opacidad del contexto; el borde de la fila
                                   // no activa dejaba alpha ~0.1 y oscurecía la portada. Forzamos 100%.
            if (cover.isValid()) g.drawImage (cover, cov, juce::RectanglePlacement::fillDestination);  // portada original (sin realce)
            else { g.setColour (juce::Colour (0xff1f1f1f)); g.fillRect (cov); }
            if (! ready) { g.setColour (juce::Colour (0x88000000)); g.fillRect (cov); }   // aún descargando / en cola: atenuar
            g.restoreState();
        }
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (cov, 6.0f, 1.0f);

        // Título a la derecha de la portada. Si la canción usa un arreglo propio,
        // el nombre sube un poco y el chip del arreglo va DEBAJO del nombre (ya no sobre la portada).
        const bool  dlActive = (dlProgress >= 0.0f && dlProgress < 1.0f);
        const float dlLabelW = (! editMode && dlActive) ? (npEsIPhone() ? 118.0f : 150.0f) : 0.0f;  // espacio para "Descargando 72%"
        const float titR  = editMode ? tonoBtnRect().getX() - 10.0f : full.getRight() - 12.0f - dlLabelW;
        const float textX = cov.getRight() + 12.0f;
        const bool  hasArr = arrNombre.isNotEmpty();
        juce::String linea = titulo;
        if (tono.isNotEmpty()) linea << "  (" << tono << ")";
        g.setColour (! ready ? juce::Colour (0xff6e6e6e)                               // atenuado mientras baja
                             : (active ? juce::Colours::white : juce::Colour (0xfff2f2f2)));
        g.setFont (juce::Font (npEsIPhone() ? 13.0f : 16.0f, juce::Font::bold));
        if (hasArr)
        {
            juce::Rectangle<float> txt (textX, full.getY() + 4.0f,
                                        juce::jmax (10.0f, titR - textX), full.getHeight() * 0.52f);
            g.drawText (linea, txt, juce::Justification::bottomLeft, true);

            juce::Font cf (npEsIPhone() ? 8.5f : 9.5f, juce::Font::bold);
            g.setFont (cf);
            const float cw = juce::jmin (titR - textX, juce::GlyphArrangement::getStringWidth (cf, arrNombre) + 14.0f);
            juce::Rectangle<float> chip (textX, txt.getBottom() + 2.0f, cw, npEsIPhone() ? 13.0f : 15.0f);
            g.setColour (juce::Colour (0xE6143018)); g.fillRoundedRectangle (chip, 5.0f);
            g.setColour (juce::Colour (0xff8fe0a0));
            g.drawText (arrNombre, chip.reduced (6.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        else
        {
            juce::Rectangle<float> txt (textX, full.getY(),
                                        juce::jmax (10.0f, titR - textX), full.getHeight());
            g.drawText (linea, txt, juce::Justification::centredLeft, true);
        }

        // Barra de descarga sobre la miniatura — SIN oscurecer la portada (solo la barrita).
        if (dlProgress >= 0.0f && dlProgress < 1.0f)
        {
            juce::Path clip; clip.addRoundedRectangle (cov, 6.0f); g.saveState(); g.reduceClipRegion (clip);
            auto bar = juce::Rectangle<float> (cov.getX() + 5.0f, cov.getBottom() - 8.0f, cov.getWidth() - 10.0f, 4.0f);
            g.setColour (juce::Colour (0x66000000)); g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (dlKind == 1 ? juce::Colour (0xffE0A43A) : juce::Colour (0xff2E6BE6));  // ámbar=transponiendo, azul=descargando
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, dlProgress)), 2.0f);
            g.restoreState();

            // Estado + porcentaje al EXTREMO DERECHO de la fila (solo fuera de edición):
            // "Transponiendo 45%" mientras el servidor rinde el tono; "Descargando 72%" al bajar el audio.
            if (! editMode)
            {
                const int pct = (int) std::lround (juce::jlimit (0.0f, 1.0f, dlProgress) * 100.0f);
                juce::String txt = (dlKind == 1 ? juce::String ("Transponiendo ") : juce::String ("Descargando ")) + juce::String (pct) + "%";
                g.setColour (dlKind == 1 ? juce::Colour (0xffE0A43A) : juce::Colour (0xff7FB0FF));
                g.setFont (juce::Font (npEsIPhone() ? 10.0f : 12.0f, juce::Font::bold));
                juce::Rectangle<float> lab (full.getRight() - 10.0f - dlLabelW, full.getY(), dlLabelW, full.getHeight());
                g.drawText (txt, lab, juce::Justification::centredRight, true);
            }
        }

        // Botones de edición a la derecha de la fila: − (quitar), ⋯ (tono), + (agregar después).
        // Estilo fino y monocromo (aro claro), sin rojo/azul.
        if (editMode)
        {
            const float glyph = npEsIPhone() ? 14.0f : 16.0f;
            const bool over = isMouseOverOrDragging();
            auto mp = getMouseXYRelative().toFloat();
            auto hov = [&] (juce::Rectangle<float> b) { return over && b.contains (mp); };
            auto ring = [&] (juce::Rectangle<float> b, const juce::String& sym)
            {
                const bool h = hov (b);
                g.setColour (juce::Colour (h ? 0x66000000 : 0x44000000)); g.fillEllipse (b);   // disco (más oscuro al hover)
                g.setColour (juce::Colour (h ? 0xffffffff : 0xcceaeaea)); g.drawEllipse (b.reduced (0.7f), h ? 1.6f : 1.3f);
                g.setColour (juce::Colour (0xffffffff)); g.setFont (juce::Font (glyph, juce::Font::bold));
                g.drawText (sym, b, juce::Justification::centred);
            };
            ring (removeBtnRect(), juce::String::fromUTF8 ("\xe2\x88\x92"));   // −
            ring (addBtnRect(),    juce::String ("+"));                        // +
            auto tb = tonoBtnRect();                                           // ⋯ (tono)
            const bool ht = hov (tb);
            g.setColour (juce::Colour (ht ? 0x66000000 : 0x44000000)); g.fillRoundedRectangle (tb, 6.0f);
            g.setColour (juce::Colour (ht ? 0x99ffffff : 0x55ffffff)); g.drawRoundedRectangle (tb.reduced (0.7f), 6.0f, ht ? 1.4f : 1.2f);
            g.setColour (juce::Colour (0xffffffff)); g.setFont (juce::Font (glyph + 2.0f, juce::Font::bold));
            g.drawText (juce::String::fromUTF8 ("\xe2\x8b\xaf"), tb, juce::Justification::centred);
        }
    }
};

struct AddCard : public juce::Component
{
    std::function<void()> onClick;
    void mouseUp (const juce::MouseEvent& e) override { if (onClick && e.mouseWasClicked()) onClick(); }
    void paint (juce::Graphics& g) override
    {
        auto full = getLocalBounds().toFloat();
        auto rowBg = full.reduced (2.0f, 2.0f);
        g.setColour (juce::Colour (0xff121212)); g.fillRoundedRectangle (rowBg, 8.0f);
        g.setColour (juce::Colour (0x55ffffff)); g.drawRoundedRectangle (rowBg, 8.0f, 1.2f);
        // Cuadro "+" a la izquierda (como la miniatura).
        auto r2 = full.reduced (0.0f, 4.0f);
        const float s = juce::jmax (10.0f, r2.getHeight());
        juce::Rectangle<float> cov (r2.getX() + 7.0f, r2.getY(), s, s);
        g.setColour (juce::Colour (0xff7Cc6ff)); g.setFont (juce::Font (s * 0.6f, juce::Font::bold));
        g.drawText ("+", cov, juce::Justification::centred);
        // Texto a la derecha.
        juce::Rectangle<float> txt (cov.getRight() + 12.0f, full.getY(), full.getRight() - (cov.getRight() + 12.0f) - 10.0f, full.getHeight());
        g.setColour (juce::Colour (0xffb8c6d8)); g.setFont (juce::Font (npEsIPhone() ? 13.0f : 15.0f, juce::Font::bold));
        g.drawText (juce::String::fromUTF8 ("Agregar canci\xc3\xb3n"), txt, juce::Justification::centredLeft, true);
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
    // Estilo botón (etiqueta) para el mixer nuevo; si buttonStyle=false usa el look clásico (círculo "S" + columna).
    bool buttonStyle = false;
    juce::String label { "S" };
    juce::Colour onCol { 0xffffffff };
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { repaint(); }
    void paint (juce::Graphics& g) override
    {
        const bool hover = isMouseOverOrDragging();
        if (buttonStyle)
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            auto bg = on ? onCol : juce::Colour (0xff1f1f1f);
            if (hover) bg = bg.brighter (0.12f);                 // sombreado al pasar el mouse
            g.setColour (bg);
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (on ? onCol.darker (0.45f) : juce::Colour (0xff3a3a3a));
            g.drawRoundedRectangle (r, 4.0f, 1.0f);
            g.setColour (on ? juce::Colour (0xff101010) : juce::Colour (0xffbcbcbc));
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (label, r, juce::Justification::centred);
            return;
        }
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
        const float rad = juce::jmin (r.getHeight() * 0.5f, 12.0f);   // pill, igual que el botón Pad
        const bool dis = ! isEnabled();                               // deshabilitado (ej. Menú en vivo): se atenúa
        g.setColour (dis ? juce::Colour (0xff151515)
                         : (active ? juce::Colour (0xff2E8BFF)
                            : (down ? juce::Colour (0xff2a2a2a) : (over ? juce::Colour (0xff262626) : juce::Colour (0xff1f1f1f)))));
        g.fillRoundedRectangle (r, rad);
        g.setColour (dis ? juce::Colour (0x12ffffff) : juce::Colour (0x22ffffff));
        g.drawRoundedRectangle (r, rad, 1.0f);

        const juce::Colour ic = dis ? juce::Colour (0xff565656)
                                    : (active ? juce::Colours::white : juce::Colour (0xffe6e6e6));
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
        else if (kind == 3)   // menú: texto "Menú" (antes 3 rayitas)
        {
            g.setColour (ic);
            g.setFont (juce::Font (juce::jmin (16.0f, b.getHeight() * 0.42f), juce::Font::bold));
            g.drawText (juce::String::fromUTF8 ("Men\xc3\xba"), b, juce::Justification::centred);
        }
        else if (kind == 4)   // repertorios: hoja con líneas (como una lista)
        {
            const float w = b.getWidth() * 0.40f, h = b.getHeight() * 0.56f;
            juce::Rectangle<float> page (b.getCentreX() - w * 0.5f, b.getCentreY() - h * 0.5f, w, h);
            g.setColour (ic);
            g.drawRoundedRectangle (page, 2.2f, 1.4f);     // contorno de la hoja
            for (int i = 0; i < 3; ++i)                    // renglones
            {
                const float yy = page.getY() + page.getHeight() * (0.28f + i * 0.22f);
                g.fillRoundedRectangle (page.getX() + 3.0f, yy - 0.9f, page.getWidth() - 6.0f, 1.8f, 0.9f);
            }
        }
        else if (kind == 5 || kind == 6)   // chevrons: 5 = arriba, 6 = abajo (navegar repertorio)
        {
            const float w  = b.getWidth() * 0.26f;
            const float cx = b.getCentreX();
            const float cy = b.getCentreY();
            const float h  = w * 0.60f;
            juce::Path p;
            if (kind == 5)   // arriba: ^
            {
                p.startNewSubPath (cx - w, cy + h * 0.5f);
                p.lineTo (cx,     cy - h * 0.5f);
                p.lineTo (cx + w, cy + h * 0.5f);
            }
            else             // abajo: v
            {
                p.startNewSubPath (cx - w, cy - h * 0.5f);
                p.lineTo (cx,     cy + h * 0.5f);
                p.lineTo (cx + w, cy - h * 0.5f);
            }
            g.setColour (ic);
            g.strokePath (p, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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

            // Siempre se descarga el ORIGINAL (t=0); la transposición se hace EN EL DISPOSITIVO.
            // e.tono queda como metadato (semitonos) para aplicar el pitch-shift al cargar.
            const auto pistasUrl = serverUrl + "/api/live/pistas/" + juce::String (e.id) + "?t=0";
            auto pv = juce::JSON::parse (httpGet (pistasUrl, token));

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

            e.folder = cacheDir.getChildFile ("song_" + juce::String (e.id) + "_t0");   // siempre el original
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
            // Limpiar temporales huerfanos de descargas cortadas (evita acumular basura).
            if (e.folder.isDirectory())
                for (auto& orphan : e.folder.findChildFiles (juce::File::findFiles, false, "*_temp*"))
                    orphan.deleteFile();
            const int N = juce::jmax (1, e.famFiles.size());
            for (int k = 0; k < e.famFiles.size(); ++k)
            {
                if (threadShouldExit()) return;
                const auto fn = e.famFiles[k];
                auto dest = e.folder.getChildFile (fn);
                if (! dest.existsAsFile() || dest.getSize() < 2000)
                {
                    const auto durl = serverUrl + "/api/live/pista/" + juce::String (e.id) + "/"
                                      + juce::URL::addEscapeChars (fn, false) + "?t=0";   // siempre el original
                    const int kk = k, ii = i;
                    bool ok = false;
                    int intento = 0;
                    // Reintenta hasta lograrlo o hasta que el usuario cierre la app: si se cae
                    // la red, NO avanza dejando la canción a medias, sino que espera y reanuda
                    // cuando vuelve la conexión (espera escalonada 2s,4s,... hasta 15s).
                    while (! ok && ! threadShouldExit())
                    {
                        ok = httpDownload (durl, token, dest,
                                [this, ii, kk, N] (double p) { progress (ii, (kk + p) / (double) N); },
                                [this] { return threadShouldExit(); });
                        if (ok || threadShouldExit()) break;
                        ++intento;
                        status (juce::String::fromUTF8 ("Sin conexi\xc3\xb3n\xe2\x80\xa6 reanudando: ") + e.titulo);
                        const int waitMs = juce::jmin (15000, 2000 * intento);
                        for (int w = 0; w < waitMs && ! threadShouldExit(); w += 200) wait (200);
                    }
                    if (! ok) return;   // salida por cierre: el repertorio queda incompleto y NO se marca como listo
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
    std::function<void()> onBack;          // vuelve al Menú
    std::function<void()> onClose;         // cierra a la pantalla principal
    juce::TextButton backBtn, closeBtn;
    juce::Rectangle<int> headerArea;

    MidiPanel()
    {
        backBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x80\xb9 Men\xc3\xba"));   // ‹ Menú
        backBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        backBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        backBtn.onClick = [this] { if (onBack) onBack(); };
        addAndMakeVisible (backBtn);

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));                  // ×
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        closeBtn.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeBtn);

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

    juce::Rectangle<int> panelBounds() const
    {
        const bool ph = npEsIPhone();
        const int rowsN = juce::jmax (1, rows.size());
        const int w = juce::jmin (ph ? getWidth() - 20 : 720, getWidth() - (ph ? 12 : 48));
        const int h = juce::jmin ((ph ? 46 : 52) + rowsN * (ph ? 40 : 46) + (ph ? 16 : 26),
                                  getHeight() - (ph ? 16 : 48));
        return getLocalBounds().withSizeKeepingCentre (w, h);
    }

    void paint (juce::Graphics& g) override
    {
        // Tarjeta centrada (no ocupa toda la pantalla).
        g.fillAll (juce::Colour (0xE6000000));               // fondo atenuado (cubre lo de atrás)
        auto p = panelBounds();
        auto pf = p.toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (pf, 14.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (pf, 14.0f, 1.2f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        g.drawText ("MIDI", headerArea, juce::Justification::centred);
        g.setColour (juce::Colour (0x18ffffff));
        g.drawLine (pf.getX() + 14.0f, (float) headerArea.getBottom(), pf.getRight() - 14.0f, (float) headerArea.getBottom(), 1.0f);
        for (int i = 1; i < rows.size(); ++i)
        {
            const float y = (float) rows[i]->name.getY() - 4.0f;
            g.setColour (juce::Colour (0x12ffffff));
            g.drawLine (pf.getX() + 18.0f, y, pf.getRight() - 18.0f, y, 1.0f);
        }
        for (int i = 0; i < rows.size(); ++i)
        {
            auto s = rows[i]->swatch.toFloat();
            g.setColour (rows[i]->noChan ? juce::Colour (0xffC9A96E) : cajaColour (i));
            g.fillEllipse (s.getCentreX() - 5.5f, s.getCentreY() - 5.5f, 11.0f, 11.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelBounds().contains (e.getPosition()) && onClose) onClose();
    }

    void resized() override
    {
        const bool ph = npEsIPhone();
        auto p = panelBounds();
        headerArea = p.removeFromTop (ph ? 46 : 52);
        backBtn.setBounds (headerArea.getX() + (ph ? 10 : 16),
                           headerArea.getCentreY() - 15,
                           ph ? 84 : 96, 30);
        closeBtn.setBounds (headerArea.getRight() - (ph ? 42 : 46),
                            headerArea.getCentreY() - 15, 34, 30);
        auto a = p.reduced (ph ? 10 : 18, ph ? 6 : 10);
        const int rh = ph ? 40 : 46;   // alto fijo por fila (compacto)
        const int ctrlH = ph ? 26 : 32;
        for (auto* r : rows)
        {
            auto row = a.removeFromTop (rh).withSizeKeepingCentre (a.getWidth(), ctrlH);
            r->swatch = row.removeFromLeft (ph ? 18 : 24);
            r->name.setBounds (row.removeFromLeft (ph ? 100 : 140));
            r->on.setBounds   (row.removeFromRight (ph ? 40 : 54));
            row.removeFromRight (ph ? 8 : 12);
            if (! r->noChan)
            {
                r->chan.setBounds (row.removeFromRight (ph ? 92 : 120));
                row.removeFromRight (ph ? 8 : 12);
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
    std::function<void()> onSaveAs;                 // duplicar el repertorio cargado (solo Mac por ahora)
    juce::TextButton newBtn, loadBtn, closeBtn, saveBtn, saveAsBtn, deleteBtn;

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

        // ── Barra fija de acciones (solo Mac por ahora): Guardar · Guardar como Nuevo · Borrar ──
        saveBtn.setButtonText ("Guardar");
        saveBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff17361f));
        saveBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff5CD98A));
        saveBtn.onClick = [this]
        {
            if (selected >= 0 && selected < items.size() && canSave (selected) && onSave)
            { savedAt = juce::Time::getMillisecondCounter(); onSave (items[selected].id); scheduleFlash(); repaint(); }
        };
        saveAsBtn.setButtonText (juce::String::fromUTF8 ("Guardar como Nuevo"));
        saveAsBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        saveAsBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff7Cc6ff));
        saveAsBtn.onClick = [this] { if (onSaveAs) onSaveAs(); };
        deleteBtn.setButtonText ("Borrar");
        deleteBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a1414));
        deleteBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe05555));
        deleteBtn.onClick = [this]
        {
            if (selected >= 0 && selected < items.size() && onDelete)
            { auto id = items[selected].id; onDelete (id); }
        };
        addAndMakeVisible (saveBtn);
        addAndMakeVisible (saveAsBtn);
        addAndMakeVisible (deleteBtn);
        // Todos con el mismo fondo; solo cambian las LETRAS (Guardar verde, Borrar rojo).
        auto styleBtn = [] (juce::TextButton& b, juce::uint32 tx)
        {
            b.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff1f1f1f));
            b.setColour (juce::TextButton::textColourOffId, juce::Colour (tx));
        };
        styleBtn (newBtn,    0xfff2f2f2);
        styleBtn (loadBtn,   0xfff2f2f2);
        styleBtn (saveBtn,   0xff5CD98A);   // verde
        styleBtn (saveAsBtn, 0xff5CD98A);   // verde también
        styleBtn (deleteBtn, 0xffe05555);   // rojo
        setAlwaysOnTop (true);
    }

    // Habilitar/deshabilitar los botones fijos según la selección (Mac).
    void updateActionButtons()
    {
        const bool sel = (selected >= 0 && selected < items.size());
        loadBtn.setEnabled (sel);
        saveBtn.setEnabled (sel && canSave (selected));
        saveAsBtn.setEnabled (currentLoadedId.isNotEmpty());
        deleteBtn.setEnabled (sel);
    }

    static constexpr int kBarN = 5;                       // botones apilados (una fila c/u)
    int barBtnH() const { return npEsIPhone() ? 34 : 44; }
    int barGap()  const { return npEsIPhone() ? 6 : 8; }
    int barH()    const { return kBarN * barBtnH() + (kBarN - 1) * barGap(); }
    int rowH()    const { return npEsIPhone() ? 46 : 54; }
    juce::Rectangle<int> panelBounds() const
    {
        const bool ph = npEsIPhone();
        const int w = ph ? 420 : 460;
        const int h = (ph ? 58 : 64) + juce::jmax (1, items.size()) * rowH() + 14 + barH() + (ph ? 12 : 18);
        return getLocalBounds().withSizeKeepingCentre (w, juce::jmin (h, getHeight() - (ph ? 16 : 40)));
    }
    juce::Rectangle<int> rowBounds (int i) const
    {
        auto p = panelBounds();
        return { p.getX() + 20, p.getY() + (npEsIPhone() ? 56 : 64) + i * rowH(), p.getWidth() - 40, npEsIPhone() ? 42 : 48 };
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

        // Encabezado: título centrado al nivel de la × y, justo debajo, el mensaje de estado (pequeño).
        {
            auto pb = panelBounds();
            juce::Rectangle<int> titleRow (pb.getX(), pb.getY() + 8, pb.getWidth(), 38);   // centro ≈ nivel de la × (y+27)
            juce::Rectangle<int> msgRow   (pb.getX(), pb.getY() + 45, pb.getWidth(), 18);  // justo arriba de los renglones
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (17.0f, juce::Font::bold));
            g.drawText ("Repertorio", titleRow, juce::Justification::centred);
            if (juce::Time::getMillisecondCounter() - savedAt < 1600)
            {
                g.setColour (juce::Colour (0xff3ED66E));
                g.setFont (juce::Font (11.5f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("\xe2\x9c\x93 Mezcla guardada"), msgRow, juce::Justification::centred);
            }
            else if (dirty)
            {
                g.setColour (juce::Colour (0xffE5484D));
                g.setFont (juce::Font (11.5f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("\xe2\x97\x8f Cambios sin guardar"), msgRow, juce::Justification::centred);
            }
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
            else if (items[i].cached)       { cbg = juce::Colour (0xff15304f); cfg = juce::Colour (0xff8fbdf0); ctxt = juce::String::fromUTF8 ("\xe2\x9c\x93 Offline"); }   // azul (antes verde)
            else                            { cbg = juce::Colour (0xff12203c); cfg = juce::Colour (0xff7Cc6ff); ctxt = juce::String::fromUTF8 ("\xe2\xa4\x93 Descargar"); }
            g.setColour (cbg); g.fillRoundedRectangle (ch, 7.0f);
            g.setColour (cfg); g.setFont (juce::Font (11.5f, juce::Font::bold));
            g.drawText (ctxt, ch, juce::Justification::centred);
        }
    }

    void resized() override { layoutButtons(); }
    void layoutButtons()
    {
        auto p = panelBounds();
        closeBtn.setBounds (p.getRight() - 46, p.getY() + 12, 34, 30);
        // Un botón por fila, apilados a lo ancho de la ventana (iPhone, iPad y Mac).
        const int bh = barBtnH(), gap = barGap();
        auto col = juce::Rectangle<int> (p.getX() + 20, p.getBottom() - (npEsIPhone() ? 12 : 18) - barH(),
                                         p.getWidth() - 40, barH());
        juce::TextButton* bar[kBarN] = { &newBtn, &loadBtn, &saveBtn, &saveAsBtn, &deleteBtn };
        for (int i = 0; i < kBarN; ++i)
        {
            bar[i]->setBounds (col.removeFromTop (bh));
            if (i < kBarN - 1) col.removeFromTop (gap);
        }
        updateActionButtons();
    }

    void scheduleFlash()
    {
        juce::Component::SafePointer<RepertoirePicker> sp (this);
        juce::Timer::callAfterDelay (1650, [sp] { if (sp) sp->repaint(); });
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < items.size(); ++i)   // chip de descarga a la derecha de la fila
            if (chipRect (i).contains (e.getPosition()))
            { if (items[i].dlPct < 0 && ! items[i].cached && onDownload) onDownload (items[i].id); return; }
        for (int i = 0; i < items.size(); ++i)   // tocar una fila la selecciona (los botones actúan sobre ella)
            if (rowBounds (i).contains (e.getPosition()))
            { selected = i; updateActionButtons(); repaint(); return; }
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
    juce::TextButton freeBtn, autoBtn, capMinus, capPlus, closeBtn, backBtn;
    juce::int64 total = 0, unused = 0;
    bool autoClean = false;
    int capGB = 0;                 // 0 = sin límite
    juce::Rectangle<int> capLblBounds, capValBounds, autoHdrBounds, autoDescBounds;
    std::function<void()> onFreeUnused;
    std::function<void (bool)> onAutoClean;
    std::function<void (int)> onCap;
    std::function<void()> onBack;                  // volver al Menú

    StoragePanel()
    {
        auto st = [] (juce::TextButton& b, juce::uint32 bg, juce::uint32 tx)
        { b.setColour (juce::TextButton::buttonColourId, juce::Colour (bg));
          b.setColour (juce::TextButton::textColourOffId, juce::Colour (tx)); };
        st (freeBtn, 0xff1f1f1f, 0xfff2f2f2);
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
        st (backBtn, 0xff1f1f1f, 0xfff2f2f2); backBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x80\xb9 Men\xc3\xba"));   // ‹ Menú
        backBtn.onClick = [this] { setVisible (false); if (onBack) onBack(); };
        addAndMakeVisible (backBtn);
        setAlwaysOnTop (true);
        refresh();
    }

    void setStats (juce::int64 t, juce::int64 u, bool aut, int cap)
    { total = t; unused = u; autoClean = aut; capGB = cap; refresh(); }

    void refresh()
    {
        freeBtn.setButtonText (juce::String::fromUTF8 ("Liberar sin usar  (") + npFmtBytes (unused) + ")");
        autoBtn.setButtonText (autoClean ? juce::String::fromUTF8 ("Activada") : juce::String::fromUTF8 ("Desactivada"));
        autoBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (autoClean ? 0xff15304f : 0xff1f1f1f));   // azul (antes verde)
        autoBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (autoClean ? 0xff8fbdf0 : 0xfff2f2f2));
        freeBtn.setEnabled (unused > 0);
        repaint();
    }

    juce::Rectangle<int> panelBounds() const
    {
        const bool ph = npEsIPhone();
        return getLocalBounds().withSizeKeepingCentre (ph ? 440 : 440,
                                                       ph ? juce::jmin (446, getHeight() - 10) : 446);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xC0000000));
        auto pf = panelBounds().toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (pf, 14.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (pf, 14.0f, 1.2f);
        auto in = panelBounds().reduced (24, 0);
        g.setColour (juce::Colours::white); g.setFont (juce::Font (17.0f, juce::Font::bold));
        g.drawText (juce::String::fromUTF8 ("Almacenamiento"), in.removeFromTop (52).withTrimmedLeft (80), juce::Justification::centredLeft);
        g.setFont (15.0f); g.setColour (juce::Colour (0xffe8e8e8));
        g.drawText (juce::String::fromUTF8 ("Total en cach\xc3\xa9:   ") + npFmtBytes (total), in.removeFromTop (26), juce::Justification::centredLeft);
        g.setFont (13.5f); g.setColour (juce::Colour (0xffa3a3a3));
        g.drawText (juce::String::fromUTF8 ("En uso por tus repertorios:   ") + npFmtBytes (total - unused), in.removeFromTop (22), juce::Justification::centredLeft);
        g.drawText (juce::String::fromUTF8 ("Sin usar (se puede liberar):   ") + npFmtBytes (unused), in.removeFromTop (22), juce::Justification::centredLeft);

        // ── sección: Limpieza automática (agrupa el interruptor + el límite) ──
        g.setColour (juce::Colour (0x18ffffff));
        g.fillRect (juce::Rectangle<int> (in.getX(), autoHdrBounds.getY() - 8, in.getWidth(), 1));
        g.setColour (juce::Colour (0xff9aa0a6)); g.setFont (juce::Font (12.0f, juce::Font::bold));
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
        backBtn.setBounds (p.getX() + 14, p.getY() + 13, 76, 28);
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
        g.setColour (on ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff4a4a4a));   // azul (antes verde)
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

// Botón "Acerca de" con un icono de info (círculo con "i") a la par del texto.
struct FlagReportButton : public juce::Button
{
    FlagReportButton() : juce::Button ("Acerca de") {}
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (down ? 0xff2a2a2a : (over ? 0xff242424 : 0xff1f1f1f)));
        g.fillRoundedRectangle (r, 8.0f);
        const juce::String label (juce::String::fromUTF8 ("Acerca de"));
        juce::Font f (juce::FontOptions (15.0f, juce::Font::bold));
        g.setFont (f);
        const float tw = juce::GlyphArrangement::getStringWidth (f, label);
        const float iconW = 16.0f, gap = 9.0f;
        float x = (getWidth() - (iconW + gap + tw)) * 0.5f;
        const float cy = getHeight() * 0.5f;
        const juce::Colour col (0xfff2f2f2);
        // icono de info: círculo con "i"
        juce::Rectangle<float> ic (x, cy - iconW * 0.5f, iconW, iconW);
        g.setColour (col);
        g.drawEllipse (ic.reduced (1.0f), 1.5f);
        g.setFont (juce::Font (juce::FontOptions (iconW * 0.72f, juce::Font::bold)));
        g.drawText ("i", ic, juce::Justification::centred);
        // texto
        g.setFont (f);
        g.setColour (col);
        g.drawText (label, juce::Rectangle<float> (x + iconW + gap, 0.0f, tw + 6.0f, (float) getHeight()),
                    juce::Justification::centredLeft);
    }
};

// ── Ventana "Acerca de": tarjeta centrada con 4 opciones ──
struct AboutPanel : public juce::Component
{
    juce::TextButton soporteBtn, emailBtn, manualBtn, webBtn, backBtn, closeBtn;
    std::function<void()> onSoporte, onEmail, onManual, onWeb, onBack, onClose;
    juce::Rectangle<int> headerArea;

    AboutPanel()
    {
        auto st = [this] (juce::TextButton& b, const char* t)
        {
            b.setButtonText (juce::String::fromUTF8 (t));
            b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
            b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
            addAndMakeVisible (b);
        };
        st (soporteBtn, "Contactar con soporte"); soporteBtn.onClick = [this] { if (onSoporte) onSoporte(); };
        st (emailBtn,   "Enviar correo a soporte"); emailBtn.onClick = [this] { if (onEmail) onEmail(); };
        st (manualBtn,  "Manual de Usuario");      manualBtn.onClick = [this] { if (onManual) onManual(); };
        st (webBtn,     "NeuralWorship.com");       webBtn.onClick   = [this] { if (onWeb) onWeb(); };

        backBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x80\xb9 Men\xc3\xba"));
        backBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        backBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        backBtn.onClick = [this] { if (onBack) onBack(); };
        addAndMakeVisible (backBtn);

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        closeBtn.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeBtn);
    }

    juce::Rectangle<int> panelBounds() const
    {
        const bool ph = npEsIPhone();
        const int w = juce::jmin (ph ? getWidth() - 20 : 460, getWidth() - (ph ? 12 : 48));
        const int h = (ph ? 46 : 52) + 4 * (ph ? 48 : 54) + (ph ? 16 : 24);
        return getLocalBounds().withSizeKeepingCentre (w, h);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xE6000000));
        auto p = panelBounds(); auto pf = p.toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (pf, 14.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (pf, 14.0f, 1.2f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        g.drawText (juce::String::fromUTF8 ("Acerca de"), headerArea, juce::Justification::centred);
        g.setColour (juce::Colour (0x18ffffff));
        g.drawLine (pf.getX() + 14.0f, (float) headerArea.getBottom(), pf.getRight() - 14.0f, (float) headerArea.getBottom(), 1.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    { if (! panelBounds().contains (e.getPosition()) && onClose) onClose(); }

    void resized() override
    {
        const bool ph = npEsIPhone();
        auto p = panelBounds();
        headerArea = p.removeFromTop (ph ? 46 : 52);
        backBtn.setBounds  (headerArea.getX() + (ph ? 10 : 16), headerArea.getCentreY() - 15, ph ? 84 : 96, 30);
        closeBtn.setBounds (headerArea.getRight() - (ph ? 42 : 46), headerArea.getCentreY() - 15, 34, 30);
        auto a = p.reduced (ph ? 12 : 18, ph ? 8 : 12);
        const int bh = ph ? 42 : 46, gap = ph ? 6 : 8;
        for (auto* b : { &soporteBtn, &emailBtn, &manualBtn, &webBtn })
        { b->setBounds (a.removeFromTop (bh)); a.removeFromTop (gap); }
    }
};

struct SettingsPanel : public juce::Component
{
    juce::TextButton syncBtn, cfgBtn, refreshBtn, storeBtn, countInBtn, masterPSBtn, mixPSBtn, closeBtn, logoutBtn, midiBtn;
    FlagReportButton reportBtn;
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
    std::function<void()> onMidi;
    std::function<void()> onReportar;
    std::function<void()> onLogout;
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
        addChildComponent (storeBtn);   // OCULTO: quitamos el control de almacenamiento (el borrado ahora es automático al eliminar canción/repertorio)

        midiBtn.setButtonText (juce::String::fromUTF8 ("MIDI"));
        midiBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        midiBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        midiBtn.onClick = [this] { if (onMidi) onMidi(); };
        addAndMakeVisible (midiBtn);

        reportBtn.onClick = [this] { if (onReportar) onReportar(); };
        addAndMakeVisible (reportBtn);

        logoutBtn.setButtonText (juce::String::fromUTF8 ("Cerrar sesión"));
        logoutBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        logoutBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe05555));   // letras rojas
        logoutBtn.onClick = [this] { if (onLogout) onLogout(); };
        addAndMakeVisible (logoutBtn);

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
        syncBtn.setButtonText ("NeuralCharts");
        syncBtn.setToggleState (syncOn, juce::dontSendNotification);

        countInBtn.setButtonText (juce::String::fromUTF8 ("Pre-roll por secci\xc3\xb3n"));
        countInBtn.setToggleState (countInOn, juce::dontSendNotification);

        masterPSBtn.setButtonText (juce::String::fromUTF8 ("Master por canci\xc3\xb3n"));
        masterPSBtn.setToggleState (masterPSOn, juce::dontSendNotification);

        mixPSBtn.setButtonText (juce::String::fromUTF8 ("Buses y mute por canci\xc3\xb3n"));
        mixPSBtn.setToggleState (mixPSOn, juce::dontSendNotification);
        repaint();
    }

    juce::Rectangle<int> panelBounds() const
    {
        const bool ph = npEsIPhone();
        if (ph)
        {
            // iPhone: la tarjeta se ajusta a la altura del contenido (sin hueco abajo).
            // MISMAS medidas que resized(): pad 15, título 44, 9 filas de 28, gap 6, status 16.
            const int h = juce::jmin (15 + 44 + 9 * 28 + 9 * 6 + 16 + 15, getHeight() - 12);
            return getLocalBounds().withSizeKeepingCentre (360, h);
        }
        return getLocalBounds().withSizeKeepingCentre (380, 606);
    }

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

        // (La señal de estado de NeuralSync ahora se muestra bajo el botón Play, no aquí.)
    }

    void resized() override
    {
        auto p = panelBounds();
        closeBtn.setBounds (p.getRight() - 46, p.getY() + 12, 34, 30);
        const bool ph = npEsIPhone();
        auto b = p.reduced (ph ? 15 : 24); b.removeFromTop (ph ? 44 : 44);
        const int bh = ph ? 28 : 46, gap = ph ? 6 : 12;
        syncBtn.setBounds     (b.removeFromTop (bh)); b.removeFromTop (gap);
        countInBtn.setBounds  (b.removeFromTop (bh)); b.removeFromTop (gap);
        masterPSBtn.setBounds (b.removeFromTop (bh)); b.removeFromTop (gap);
        mixPSBtn.setBounds    (b.removeFromTop (bh)); b.removeFromTop (gap);
        cfgBtn.setBounds      (b.removeFromTop (bh)); b.removeFromTop (gap);
        midiBtn.setBounds     (b.removeFromTop (bh)); b.removeFromTop (gap);
        refreshBtn.setBounds  (b.removeFromTop (bh)); b.removeFromTop (gap);
        reportBtn.setBounds   (b.removeFromTop (bh)); b.removeFromTop (gap);
        logoutBtn.setBounds   (b.removeFromTop (bh)); b.removeFromTop (gap);
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
    juce::Viewport famView;             // contenedor scrollable de las familias (arrastre táctil)
    juce::Component famContent;         // contenido desplazable (labels + combos)
    juce::TextButton closeBtn, backBtn;
    juce::ComboBox srBox;                  // selector de frecuencia (sample rate)
    juce::Label srLbl;
    juce::Array<double> srList;
    juce::ToggleButton autoPanBtn;         // Autopan para jack de 2 salidas
    std::function<void (bool)> onAutoPan;
    void setAutoPan (bool on) { autoPanBtn.setToggleState (on, juce::dontSendNotification); }
    int numChans = 2;
    int maxChans = 32;                              // tope de salidas segun el plan (2 en Basico)
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
            l->setFont (juce::Font (13.0f)); famContent.addAndMakeVisible (l);
            auto* c = routeBoxes.add (new juce::ComboBox()); dark (*c);
            const int fi = i;
            c->onChange = [this, fi] { fireRoute (fi); };
            famContent.addAndMakeVisible (c);
        }
        // Viewport scrollable para las familias (arrastre con el dedo en cualquier parte)
        famView.setViewedComponent (&famContent, false);
        famView.setScrollBarsShown (true, false);
        famView.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::nonHover);
        famView.setScrollBarThickness (8);
        addAndMakeVisible (famView);
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

        // Autopan: 1 toque para mandar Click/Guia a la derecha y las pistas a la izquierda
        // (ideal para quien solo tiene un jack de 2 salidas y no quiere rutear familia por familia).
        autoPanBtn.setButtonText (juce::String::fromUTF8 ("Autopan  \xc2\xb7  Click y Gu\xc3\xad" "a a la derecha, pistas a la izquierda"));
        autoPanBtn.setColour (juce::ToggleButton::textColourId, juce::Colour (0xfff2f2f2));
        autoPanBtn.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xff2E6BE6));
        autoPanBtn.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0x55ffffff));
        autoPanBtn.onClick = [this] { if (onAutoPan) onAutoPan (autoPanBtn.getToggleState()); };
        addAndMakeVisible (autoPanBtn);
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
        numChans = juce::jlimit (0, juce::jmin (32, maxChans), chans);
        chInfo.setText (juce::String (numChans) + juce::String::fromUTF8 (" canales disponibles")
                        + (maxChans <= 2 ? juce::String::fromUTF8 (" \xc2\xb7 plan B\xc3\xa1sico") : juce::String()),
                        juce::dontSendNotification);
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

    juce::Rectangle<int> panelBounds() const
    {
        const bool ph = npEsIPhone();
        return getLocalBounds().withSizeKeepingCentre (ph ? 480 : 480,
                                                       ph ? juce::jmin (624, getHeight() - 10) : 624);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xC0000000));
        auto p = panelBounds().toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (p, 14.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (p, 14.0f, 1.2f);
    }

    void resized() override
    {
        const bool ph = npEsIPhone();
        auto p = panelBounds();
        closeBtn.setBounds (p.getRight() - 46, p.getY() + 12, 34, 30);
        backBtn.setBounds (p.getX() + 14, p.getY() + 13, 76, 28);
        auto b = p.reduced (ph ? 16 : 22);
        title.setBounds (p.getX() + 96, p.getY() + 12, p.getWidth() - 148, 30);   // alineado con ‹Menú y ×
        b.removeFromTop (ph ? 30 : 34);   // reservar la cabecera en el flujo
        b.removeFromTop (ph ? 2 : 4);
        devLbl.setBounds (b.removeFromTop (ph ? 14 : 16));
        deviceBox.setBounds (b.removeFromTop (ph ? 28 : 30));
        chInfo.setBounds (b.removeFromTop (ph ? 16 : 20));
        b.removeFromTop (ph ? 4 : 6);
        srLbl.setBounds (b.removeFromTop (ph ? 14 : 16));
        srBox.setBounds (b.removeFromTop (ph ? 28 : 30));
        b.removeFromTop (ph ? 5 : 8);
        autoPanBtn.setBounds (b.removeFromTop (ph ? 24 : 26));
        b.removeFromTop (ph ? 5 : 8);

        // Lista de familias dentro de un Viewport scrollable (arrastre táctil en cualquier parte).
        famView.setBounds (b);
        const int rowH = ph ? 28 : 32, rowGap = ph ? 3 : 4, pitch = rowH + rowGap;
        const int totalH = kNumFam * pitch;
        const bool needsScroll = totalH > b.getHeight();
        const int cw = b.getWidth() - (needsScroll ? 10 : 0);   // deja espacio a la barra si scrollea
        famContent.setSize (cw, totalH);
        for (int i = 0; i < kNumFam; ++i)
        {
            juce::Rectangle<int> row (0, i * pitch, cw, rowH);
            famLabels[i]->setBounds (row.removeFromLeft (ph ? 130 : 160));
            routeBoxes[i]->setBounds (row.reduced (0, 2));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelBounds().contains (e.getPosition())) setVisible (false);
    }
};

// Campo de texto con esquinas redondeadas (para el buscador de canciones).
struct NPRoundEditLnF : public juce::LookAndFeel_V4
{
    void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& te) override
    {
        g.setColour (te.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, 11.0f);
    }
    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& te) override
    {
        const bool foc = te.hasKeyboardFocus (true);
        g.setColour (te.findColour (foc ? juce::TextEditor::focusedOutlineColourId
                                        : juce::TextEditor::outlineColourId));
        g.drawRoundedRectangle (0.6f, 0.6f, (float) w - 1.2f, (float) h - 1.2f, 11.0f, foc ? 1.4f : 1.0f);
    }
};

static juce::Font npFontMarca (float alturaPx, bool bold);   // definida más abajo

// LookAndFeel del selector de tono: caja redondeada compacta + menú fino y oscuro.
struct NPTonoLnF : public juce::LookAndFeel_V4
{
    NPTonoLnF()
    {
        setColour (juce::PopupMenu::backgroundColourId,            juce::Colour (0xff1b1b1b));
        setColour (juce::PopupMenu::textColourId,                  juce::Colour (0xffe8e8e8));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff2e2e2e));
        setColour (juce::PopupMenu::highlightedTextColourId,       juce::Colours::white);
    }
    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f);
        g.setColour (juce::Colour (0xff1c1c1c)); g.fillRoundedRectangle (r, 10.0f);
        g.setColour (box.isPopupActive() ? juce::Colour (0x66ffffff) : juce::Colour (0x33ffffff));
        g.drawRoundedRectangle (r, 10.0f, 1.0f);
        const float cx = (float) w - 17.0f, cy = (float) h * 0.5f;   // chevron ▾
        juce::Path p; p.startNewSubPath (cx - 5.0f, cy - 3.0f);
        p.lineTo (cx, cy + 3.0f); p.lineTo (cx + 5.0f, cy - 3.0f);
        g.setColour (juce::Colour (0xffb0b0b0)); g.strokePath (p, juce::PathStrokeType (1.6f));
    }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return npFontMarca (16.0f, true); }
    void positionComboBoxText (juce::ComboBox& box, juce::Label& lbl) override
    {
        lbl.setBounds (12, 1, box.getWidth() - 30, box.getHeight() - 2);
        lbl.setFont (npFontMarca (16.0f, true));
        lbl.setJustificationType (juce::Justification::centred);
    }
    juce::Font getPopupMenuFont() override { return npFontMarca (15.0f, false); }
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                    int stdHeight, int& idealW, int& idealH) override
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, stdHeight, idealW, idealH);
        if (! isSeparator) idealH = npEsIPhone() ? 30 : 26;   // filas compactas
    }
    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.setColour (juce::Colour (0xff1b1b1b));
        g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, 10.0f);
        g.setColour (juce::Colour (0x22ffffff));
        g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, 10.0f, 1.0f);
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
    int bibDragStartY = 0, bibScrollStart = 0;   // scroll táctil (arrastre con el dedo)
    bool bibDragging = false, bibMoved = false;
    // Scroll vertical del diálogo de Tono (iPhone horizontal: el contenido no cabe de alto).
    int tonoScroll = 0, tonoDragStartY = 0, tonoScrollStart = 0;
    bool tonoDragging = false, tonoMoved = false;
    NPRoundEditLnF searchLnf;          // buscador con esquinas redondeadas
    juce::TextEditor searchBox;

    int songId = 0; juce::String songTitle, songArtist; bool addFlow = false;
    struct Key { juce::String nombre; int sem = 0; bool rendered = false; };
    juce::Array<Key> keys;
    int renderingSem = 99, pendIdx = -1, progHechos = 0, progTotal = 0;
    NPTonoLnF tonoLnf;             // estilo del menú de tono
    int selTonoIdx = 0;            // índice del tono elegido en la caja (aún sin confirmar)
    juce::StringArray arrNames;    // arreglos guardados de la canción (sin "Original")
    int selArrIdx = 0;             // 0 = Original; 1.. = arrNames[idx-1]
    juce::String selectedArrName() const   // nombre del arreglo elegido ("Original" = sin arreglo)
    { return (selArrIdx <= 0 || selArrIdx - 1 >= arrNames.size()) ? juce::String::fromUTF8 ("Original") : arrNames[selArrIdx - 1]; }
    // Guía: 0 = Tracks (pista que subió el usuario) · 1 = Neural (guía dinámica por voz)
    int  guiaMode = 0;
    bool repTrack = false;   // permitir repetir/∞ en modo Track (opción con advertencia)
    bool guiaForzadaNeural() const { return selArrIdx != 0; }   // cualquier arreglo obliga guía Neural
    int  guiaEfectiva() const { return guiaForzadaNeural() ? 1 : guiaMode; }
    juce::TextButton cargarBtn;    // confirmación: aplicar el tono elegido
    int prepFrame = 0;             // animación del spinner "Preparando…"

    juce::TextButton closeBtn, backBtn;
    std::function<void (int)> onPickSong;            // biblioteca -> elegir cancion
    std::function<void (int, juce::String)> onChoose; // (semitonos, nombre) -> aplicar
    std::function<void (int, juce::String, bool)> onElegir; // (sem, nombre, ya_listo): cerrar y mostrar barra en la portada
    std::function<void (int, juce::String)> onDeleteArr;    // (songId, nombreArreglo): borrar un arreglo guardado
    std::function<void (int, juce::String)> onRenameArr;    // (songId, nombreArreglo): renombrar un arreglo guardado

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
            b.setColour (juce::TextButton::buttonColourId, on ? juce::Colour (0xff15304f) : juce::Colour (0xff1f1f1f));   // azul al seleccionar (antes verde)
            b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff2E8BFF));
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
        searchBox.setLookAndFeel (&searchLnf);           // esquinas redondeadas
        searchBox.setJustification (juce::Justification::centredLeft);
        searchBox.setIndents (12, 0);
        searchBox.onTextChange = [this] { applyFilter(); };
        addChildComponent (searchBox);

        // Botón "Cargar": confirma el tono elegido en el dropdown (no se aplica al seleccionar).
        cargarBtn.setButtonText (juce::String::fromUTF8 ("Cargar"));
        cargarBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2E6BE6));
        cargarBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        cargarBtn.onClick = [this]
        {
            if (selTonoIdx < 0 || selTonoIdx >= keys.size()) return;
            auto k = keys.getReference (selTonoIdx);
            setVisible (false);
            if (onElegir) onElegir (k.sem, k.nombre, k.rendered);
        };
        addChildComponent (cargarBtn);

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
                   bool pIntro = false, bool pOutro = false, int curSem = 0,
                   juce::StringArray arrs = {}, int curArrIdx = 0, int guiaSel = 0, bool repSel = false)
    { mode = Tono; songId = id; songTitle = title; addFlow = add; keys = std::move (ks); searchBox.setVisible (false);
      arrNames = arrs; selArrIdx = juce::jlimit (0, arrs.size(), curArrIdx);   // 0 = Original
      guiaMode = (guiaSel == 1) ? 1 : 0;   // Tracks por defecto; Neural si venía elegido
      repTrack = repSel;                   // permitir repetir/∞ en Track
      tonoScroll = 0; tonoDragging = false;   // abrir el diálogo desde arriba
      songArtist.clear(); for (auto& b : bibAll) if (b.id == id) { songArtist = b.artista; break; }   // artista para el subtítulo
      inOn = (inSec >= 0.0); outOn = (outSec >= 0.0);
      padIn = pIntro; padOut = pOutro;
      inEdit.setText (secsToMMSS (inSec >= 0.0 ? inSec : 0.0), false);
      outEdit.setText (secsToMMSS (outSec >= 0.0 ? outSec : 0.0), false);
      refreshInOut();
      // Tono mostrado: el tono actual de la canción (curSem); si no hay, el original (sem 0).
      selTonoIdx = 0;
      for (int i = 0; i < keys.size(); ++i) if (keys[i].sem == curSem) { selTonoIdx = i; break; }
      renderingSem = 99; stopTimer(); resized(); repaint(); }

    void showBiblioteca()   // volver del grid de tonos a la lista de canciones
    { mode = Biblioteca; searchBox.setVisible (true); refreshInOut(); renderingSem = 99; stopTimer(); resized(); repaint(); }

    bool phLand() const { return npEsIPhone() && getWidth() > getHeight(); }   // iPhone horizontal (pantalla baja)
    juce::Rectangle<int> panelBounds() const
    {
        if (phLand())   // horizontal: tarjeta compacta de 2 columnas, con margen alrededor
        {
            const int w = juce::jmin (720, getWidth() - 48);
            const int h = juce::jmin (getHeight() - 18, (mode == Tono ? tonoContentH() + 48 : 420));
            return getLocalBounds().withSizeKeepingCentre (w, h);
        }
        const int h = npEsIPhone() ? juce::jmin (636, getHeight() - 10) : 636;
        return getLocalBounds().withSizeKeepingCentre (470, h);
    }
    // Base del contenido del diálogo de Tono, desplazada por el scroll (iPhone horizontal).
    juce::Rectangle<int> tp() const { return panelBounds().reduced(22).translated (0, -tonoScroll); }
    int tonoContentH() const   // alto total del contenido (tono+guía+cargar+inicio/fin+pad)
    {
        const bool ph = npEsIPhone();
        if (phLand()) return rpTop() + topBlockH() + rpGapGrid() + 112;   // inicio/fin y Pad van en 2 columnas (misma altura)
        return rpTop() + topBlockH() + rpGapGrid() + (ph ? 120 : 124) + (ph ? 16 : 14) + (ph ? 100 : 96);
    }
    int tonoMaxScroll() const
    {
        if (mode != Tono) return 0;
        const int avail = panelBounds().getHeight() - 22 - 10;   // margen arriba/abajo de la tarjeta
        const int ms = tonoContentH() - avail;
        return ms > 10 ? ms : 0;   // desbordes mínimos (Mac/iPad/portrait) no activan scroll
    }
    // Offsets verticales compactos en iPhone para que todo el contenido quepa.
    int rpTop()   const { return npEsIPhone() ? 42 : 58; }        // debajo del título
    int rpCh()    const { return npEsIPhone() ? 30 : 46; }        // alto de cada botón de tono (combos/pills más bajos en iPhone)
    int rpGap()   const { return npEsIPhone() ? 6  : 9;  }        // separación de la grilla
    int rpPitch() const { return rpCh() + rpGap(); }             // paso de fila
    int rpGapGrid() const { return npEsIPhone() ? 12 : 20; }      // debajo de la grilla

    juce::Rectangle<int> keyRect (int i) const
    {
        auto p = tp(); p.removeFromTop (rpTop());
        const int cols = 4, gap = rpGap(), cw = (p.getWidth() - (cols - 1) * gap) / cols, ch = rpCh();
        return { p.getX() + (i % cols) * (cw + gap), p.getY() + (i / cols) * (ch + gap), cw, ch };
    }
    int tonoLabelH() const { return npEsIPhone() ? 18 : 24; }   // "Tono" encima del dropdown
    int tonoBlockH() const { return tonoLabelH() + rpCh(); }     // etiqueta + combo
    int cargarGap() const { return npEsIPhone() ? 10 : 10; }
    int cargarH()   const { return npEsIPhone() ? 36 : 46; }     // alto del botón "Cargar"
    // ── Selector de Guía (Tracks / Neural), fila propia bajo tono/arreglo ──
    int guiaGapTop() const { return npEsIPhone() ? 10 : 14; }    // espacio sobre la fila de guía
    int guiaLabelH() const { return tonoLabelH(); }             // "GUÍA" encima de las pills
    int guiaNoteH()  const { return npEsIPhone() ? 14 : 17; }    // aviso bajo las pills
    int guiaRepGap() const { return npEsIPhone() ? 8 : 16; }     // aire entre el aviso y la casilla
    int guiaRepH()   const { return npEsIPhone() ? 24 : 28; }    // solo casilla + título (advertencia eliminada)
    bool showRepCheck() const { return false; }   // casilla eliminada: Repetir/∞ siempre habilitados
    // La casilla "Repetir/∞" solo ocupa espacio en modo Track; en Neural no deja hueco.
    int guiaBlockH() const { return guiaGapTop() + guiaLabelH() + rpCh() + guiaNoteH() + (showRepCheck() ? (guiaRepGap() + guiaRepH()) : 0); }
    juce::Rectangle<int> repTrackRect() const    // casilla "Permitir repetir/∞" (modo Track)
    {
        auto p = tp();
        p.removeFromTop (rpTop() + tonoBlockH() + guiaGapTop() + guiaLabelH() + rpCh() + guiaNoteH() + guiaRepGap());
        return p.removeFromTop (guiaRepH());
    }
    juce::Rectangle<int> guiaRowArea() const
    {
        auto p = tp();
        p.removeFromTop (rpTop() + tonoBlockH() + guiaGapTop() + guiaLabelH());
        return p.removeFromTop (rpCh());
    }
    juce::Rectangle<int> guiaTracksRect() const   // pill izquierda
    { auto r = guiaRowArea(); const int gap = 12; const int cw = (r.getWidth() - gap) / 2; return { r.getX(), r.getY(), cw, rpCh() }; }
    juce::Rectangle<int> guiaNeuralRect() const   // pill derecha
    { auto r = guiaRowArea(); const int gap = 12; const int cw = (r.getWidth() - gap) / 2; return { r.getRight() - cw, r.getY(), cw, rpCh() }; }
    juce::Rectangle<int> guiaNoteRect() const
    { auto r = guiaRowArea(); return { r.getX(), r.getBottom() + 1, r.getWidth(), guiaNoteH() }; }
    int topBlockH() const { return tonoBlockH() + guiaBlockH() + cargarGap() + cargarH(); }   // caja + guía + botón
    juce::Rectangle<int> tonoRowArea() const
    {
        auto p = tp(); p.removeFromTop (rpTop() + tonoLabelH());
        return p.removeFromTop (rpCh());
    }
    juce::Rectangle<int> tonoComboRect() const   // caja TONO (mitad izquierda)
    {
        auto r = tonoRowArea(); const int gap = 12; const int cw = (r.getWidth() - gap) / 2;
        return { r.getX(), r.getY(), cw, rpCh() };
    }
    juce::Rectangle<int> arrComboRect() const    // caja ARREGLO (mitad derecha)
    {
        auto r = tonoRowArea(); const int gap = 12; const int cw = (r.getWidth() - gap) / 2;
        return { r.getRight() - cw, r.getY(), cw, rpCh() };
    }
    juce::Rectangle<int> cargarBtnRect() const   // botón "Cargar" debajo de la caja + guía
    {
        auto p = tp(); p.removeFromTop (rpTop() + tonoBlockH() + guiaBlockH() + cargarGap());
        auto row = p.removeFromTop (cargarH());
        return row.withSizeKeepingCentre (juce::jmin (row.getWidth(), npEsIPhone() ? 200 : 240), cargarH());
    }
    juce::Rectangle<int> inOutArea() const   // #2 zona de inicio/fin, debajo del botón Cargar
    {
        auto p = tp();
        p.removeFromTop (rpTop() + topBlockH() + rpGapGrid());
        auto row = p.removeFromTop (phLand() ? 112 : (npEsIPhone() ? 120 : 124));
        if (phLand()) return row.removeFromLeft ((row.getWidth() - 24) / 2);   // horizontal: columna izquierda
        return row;
    }
    juce::Rectangle<int> ioRow (int row) const   // row 0 = inicio, 1 = fin
    {
        auto a = inOutArea(); a.removeFromTop (44);       // debajo del título + espacio para "min:seg"
        const int rh = npEsIPhone() ? 30 : 34, gap = npEsIPhone() ? 8 : 10;
        a.removeFromTop (row * (rh + gap));
        return a.removeFromTop (rh);
    }
    juce::Rectangle<int> ioTglRect (int row)   const { return ioRow (row).removeFromLeft (30).withSizeKeepingCentre (28, 28); }
    juce::Rectangle<int> ioLabelRect (int row) const { auto r = ioRow (row); r.removeFromLeft (38); return r.removeFromLeft (96); }
    juce::Rectangle<int> ioEditRect (int row)  const { auto r = ioRow (row); r.removeFromLeft (38 + 96); return r.removeFromLeft (90).withSizeKeepingCentre (90, 30); }
    juce::Rectangle<int> padArea() const   // Pad Player, debajo de inicio/fin (o a la derecha en horizontal)
    {
        if (phLand())   // horizontal: columna DERECHA, a la misma altura que inicio/fin
        {
            auto p = tp();
            p.removeFromTop (rpTop() + topBlockH() + rpGapGrid());
            auto row = p.removeFromTop (112);
            return row.removeFromRight ((row.getWidth() - 24) / 2);
        }
        auto p = tp();
        p.removeFromTop (rpTop() + topBlockH() + rpGapGrid() + (npEsIPhone() ? 120 : 124) + (npEsIPhone() ? 16 : 14));
        return p.removeFromTop (npEsIPhone() ? 100 : 96);
    }
    juce::Rectangle<int> padRow (int row) const
    {
        auto a = padArea(); a.removeFromTop (phLand() ? 44 : 30);   // horizontal: alinear filas con la columna de inicio/fin
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

        if (mode == Biblioteca)
        {
            g.setColour (juce::Colours::white); g.setFont (juce::Font (17.0f, juce::Font::bold));
            auto tarea = panelBounds().removeFromTop (52).reduced (22, 0).withTrimmedRight (48);
            g.drawText (juce::String::fromUTF8 ("Agregar canci\xc3\xb3n"), tarea, juce::Justification::centredLeft);
        }
        else
        {
            // Tono: el nombre de la canción como título (centrado) y el artista debajo, más chico.
            const int hy = panelBounds().getY();
            auto midX = panelBounds().reduced (104, 0);   // recorte igual a ambos lados → centrado real (libra ‹ Atrás y ×)
            g.setColour (juce::Colours::white); g.setFont (juce::Font (17.0f, juce::Font::bold));
            if (songArtist.isNotEmpty())
            {
                g.drawText (songTitle, midX.withY (hy + 9).withHeight (24), juce::Justification::centred);
                g.setColour (juce::Colour (0xffa3a3a3)); g.setFont (juce::Font (12.0f));
                g.drawText (songArtist, midX.withY (hy + 33).withHeight (16), juce::Justification::centred);
            }
            else
                g.drawText (songTitle, midX.withY (hy + 13).withHeight (28), juce::Justification::centred);
        }

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
            // Recorte SOLO arriba: al deslizar, el contenido no se monta sobre el título.
            // Abajo queda libre (los campos y el Pad se ven completos y parejos con sus textos).
            juce::Graphics::ScopedSaveState sssTono (g);
            { auto cc = getLocalBounds(); cc.setTop (panelBounds().getY() + rpTop() - 8); g.reduceClipRegion (cc); }
            if (renderingSem == 99)
            {
                // Etiqueta "TONO" + caja del selector (menú propio al tocarla).
                // Caja genérica de combo (etiqueta arriba + caja + chevron + texto)
                auto drawCombo = [&] (juce::Rectangle<int> rc, const juce::String& label, const juce::String& value)
                {
                    auto lbl = rc.translated (0, -tonoLabelH()).withHeight (tonoLabelH() - 2);
                    g.setColour (juce::Colour (0xff9aa0a6)); g.setFont (npFontMarca (12.5f, false));
                    g.drawText (label, lbl, juce::Justification::centred);
                    auto box = rc.toFloat();
                    g.setColour (juce::Colour (0xff1c1c1c)); g.fillRoundedRectangle (box, 10.0f);
                    g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (box, 10.0f, 1.0f);
                    g.setColour (juce::Colours::white); g.setFont (npFontMarca (15.0f, true));
                    g.drawText (value, rc.reduced (14, 0).withTrimmedRight (12), juce::Justification::centred);
                    const float cx = box.getRight() - 14.0f, cy = box.getCentreY();   // chevron ▾
                    juce::Path pth; pth.startNewSubPath (cx - 5.0f, cy - 3.0f);
                    pth.lineTo (cx, cy + 3.0f); pth.lineTo (cx + 5.0f, cy - 3.0f);
                    g.setColour (juce::Colour (0xffb0b0b0)); g.strokePath (pth, juce::PathStrokeType (1.6f));
                };
                const juce::String nom = (selTonoIdx >= 0 && selTonoIdx < keys.size()) ? keys[selTonoIdx].nombre : juce::String();
                drawCombo (tonoComboRect(), juce::String::fromUTF8 ("TONO"), nom);
                drawCombo (arrComboRect(),  juce::String::fromUTF8 ("ARREGLO"), selectedArrName());

                // ── Selector de Guía: dos pills (Tracks / Neural) ──
                {
                    const bool forzada = guiaForzadaNeural();
                    const int  efc = guiaEfectiva();
                    auto lbl = guiaRowArea().translated (0, -guiaLabelH()).withHeight (guiaLabelH() - 2);
                    g.setColour (juce::Colour (0xff9aa0a6)); g.setFont (npFontMarca (12.5f, false));
                    g.drawText (juce::String::fromUTF8 ("GU\xc3\x8d" "A"), lbl, juce::Justification::centred);

                    auto pill = [&] (juce::Rectangle<int> rc, const juce::String& txt, bool on, bool dim)
                    {
                        auto box = rc.toFloat();
                        g.setColour (on ? juce::Colour (0xff2E6BE6) : juce::Colour (0xff1c1c1c));
                        g.fillRoundedRectangle (box, 10.0f);
                        g.setColour (on ? juce::Colour (0x66ffffff) : juce::Colour (0x33ffffff));
                        g.drawRoundedRectangle (box, 10.0f, 1.0f);
                        g.setColour (dim ? juce::Colour (0xff5a5a5a) : (on ? juce::Colours::white : juce::Colour (0xffd0d0d0)));
                        g.setFont (npFontMarca (14.0f, on));
                        g.drawText (txt, rc.reduced (6, 0), juce::Justification::centred);
                    };
                    pill (guiaTracksRect(), juce::String::fromUTF8 ("Tracks"),  efc == 0, forzada);
                    pill (guiaNeuralRect(), juce::String::fromUTF8 ("Neural"),  efc == 1, false);

                    // Aviso como "chip" redondeado, centrado y fino (color según el estado).
                    auto drawNota = [&] (juce::Colour c, const juce::String& txt)
                    {
                        // Solo texto (sin recuadro) y con un respiro extra respecto a los botones Tracks/Neural.
                        auto area = guiaNoteRect().translated (0, 6);
                        g.setColour (c.brighter (0.15f)); g.setFont (npFontMarca (10.5f, false));
                        g.drawFittedText (txt, area.reduced (12, 0), juce::Justification::centred, 1);
                    };
                    if (forzada)
                        drawNota (juce::Colour (0xffe8e8e8),
                                  juce::String::fromUTF8 ("Los arreglos personalizados de canciones solo se permiten con nuestra gu\xc3\xad" "a din\xc3\xa1mica"));
                    else if (efc == 1)
                        drawNota (juce::Colour (0xffe8e8e8),
                                  juce::String::fromUTF8 ("Nuestra gu\xc3\xad" "a din\xc3\xa1mica lista para cualquier cambio e improvisaci\xc3\xb3n en vivo"));
                    else
                        drawNota (juce::Colour (0xffe8e8e8),
                                  juce::String::fromUTF8 ("Gu\xc3\xad" "a Tracks que subiste en la canci\xc3\xb3n"));

                    // Casilla "Habilitar botones Repetir e Infinito" eliminada: ahora siempre están habilitados.
                }
            }
            else
            {
                // Spinner limpio "Preparando tu tono…" — sin detalles técnicos.
                auto pb = tp(); pb.removeFromTop (rpTop());
                auto area = pb.removeFromTop (tonoBlockH() + rpGapGrid() + (npEsIPhone() ? 104 : 124));
                const int nd = prepFrame % 4;
                juce::String dots = nd == 0 ? juce::String() : juce::String::repeatedString (".", nd);
                g.setColour (juce::Colours::white); g.setFont (juce::Font (17.0f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("Preparando tu tono") + dots, area, juce::Justification::centred);
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
                g.drawText ("min:seg", ioEditRect (0).translated (0, -17).withHeight (14), juce::Justification::centred);

                // Pad Player (intro / outro)
                auto pttl = padArea().removeFromTop (24);
                g.setColour (juce::Colour (0xffcfcfcf)); g.setFont (juce::Font (13.0f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("Pad Player (autom\xc3\xa1tico)"), pttl, juce::Justification::centredLeft);
                g.setColour (juce::Colour (0xffe8e8e8)); g.setFont (juce::Font (13.0f));
                g.drawText (juce::String::fromUTF8 ("Pad al iniciar"), padLabelRect (0), juce::Justification::centredLeft);
                g.drawText (juce::String::fromUTF8 ("Pad al finalizar"),              padLabelRect (1), juce::Justification::centredLeft);
            }
        }
    }

    void resized() override
    {
        tonoScroll = juce::jlimit (0, tonoMaxScroll(), tonoScroll);   // acotar scroll del diálogo
        closeBtn.setBounds (panelBounds().getRight() - 46, panelBounds().getY() + 12, 34, 30);   // × siempre arriba-der
        backBtn.setVisible (mode == Tono && addFlow);   // ‹ Atrás solo si venís del flujo de AGREGAR, no al editar
        backBtn.setBounds (panelBounds().getX() + 14, panelBounds().getY() + 12, 96, 30);
        searchBox.setBounds (searchRect());

        cargarBtn.setBounds (cargarBtnRect());
        cargarBtn.setVisible (mode == Tono);

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
        bibDragging = false; bibMoved = false;
        // Tono: armar posible arrastre de scroll (si el contenido no cabe de alto).
        if (mode == Tono) { tonoDragStartY = e.y; tonoScrollStart = tonoScroll; tonoMoved = false; tonoDragging = (tonoMaxScroll() > 0); }
        if (mode == Biblioteca)
        {
            // Solo registrar el arranque; la selección se hace en mouseUp si NO hubo arrastre
            // (así el dedo puede desplazar la lista sin abrir una canción).
            if (bibListArea().contains (e.getPosition()))
            { bibDragging = true; bibDragStartY = e.y; bibScrollStart = bibScroll; }
        }
        else if (tonoComboRect().contains (e.getPosition()))
            showTonoMenu();
        else if (arrComboRect().contains (e.getPosition()))
            showArrMenu();
        else if (guiaTracksRect().contains (e.getPosition()))
        {   // Tracks solo disponible en Original; con arreglo se queda en Neural
            if (! guiaForzadaNeural()) { guiaMode = 0; resized(); repaint(); }   // resized: reubica campos al aparecer la casilla
        }
        else if (guiaNeuralRect().contains (e.getPosition()))
        { guiaMode = 1; resized(); repaint(); }
    }

    void showArrMenu()
    {
        juce::PopupMenu m;
        m.setLookAndFeel (&tonoLnf);
        m.addItem (1, juce::String::fromUTF8 ("Original"), true, selArrIdx == 0);   // 1 => idx 0
        for (int i = 0; i < arrNames.size(); ++i)
            m.addItem (i + 2, arrNames[i], true, selArrIdx == i + 1);               // i+2 => idx i+1
        if (arrNames.size() > 0)   // submenús para renombrar (2000+) y borrar (1000+) arreglos
        {
            m.addSeparator();
            juce::PopupMenu ren;
            for (int i = 0; i < arrNames.size(); ++i)
                ren.addItem (2000 + i, arrNames[i]);
            m.addSubMenu (juce::String::fromUTF8 ("Renombrar arreglo"), ren);
            juce::PopupMenu del;
            for (int i = 0; i < arrNames.size(); ++i)
                del.addItem (1000 + i, arrNames[i]);
            m.addSubMenu (juce::String::fromUTF8 ("Borrar arreglo"), del);
        }
        auto scr = localAreaToGlobal (arrComboRect());
        juce::Component::SafePointer<RepEditPanel> sp (this);
        m.showMenuAsync (juce::PopupMenu::Options()
                             .withTargetScreenArea (scr)
                             .withMinimumWidth (arrComboRect().getWidth())
                             .withMaximumNumColumns (1)
                             .withStandardItemHeight (npEsIPhone() ? 30 : 26),
            [sp] (int r)
            {
                if (sp == nullptr || r <= 0) return;
                if (r >= 2000)   // renombrar el arreglo r-2000
                {
                    const int ri = r - 2000;
                    if (ri < 0 || ri >= sp->arrNames.size()) return;
                    if (sp->onRenameArr) sp->onRenameArr (sp->songId, sp->arrNames[ri]);
                    return;   // la caja se refresca desde el contenedor tras renombrar
                }
                if (r >= 1000)   // borrar el arreglo r-1000
                {
                    const int di = r - 1000;
                    if (di < 0 || di >= sp->arrNames.size()) return;
                    const juce::String nom = sp->arrNames[di];
                    if (sp->onDeleteArr) sp->onDeleteArr (sp->songId, nom);
                    sp->arrNames.remove (di);                       // quitar de la caja al instante
                    if (sp->selArrIdx == di + 1) sp->selArrIdx = 0; // si estaba elegido, volver a Original
                    else if (sp->selArrIdx > di + 1) sp->selArrIdx -= 1;
                    sp->repaint();
                    return;
                }
                sp->selArrIdx = r - 1;   // 1->0 (Original), 2->1, ...
                sp->resized();           // un arreglo fuerza Neural → cambia si se muestra la casilla
                sp->repaint();
            });
    }

    void showTonoMenu()
    {
        juce::PopupMenu m;
        m.setLookAndFeel (&tonoLnf);
        for (int i = 0; i < keys.size(); ++i)
            m.addItem (i + 1, keys[i].nombre, true, i == selTonoIdx);
        auto scr = localAreaToGlobal (tonoComboRect());
        juce::Component::SafePointer<RepEditPanel> sp (this);
        m.showMenuAsync (juce::PopupMenu::Options()
                             .withTargetScreenArea (scr)
                             .withMinimumWidth (tonoComboRect().getWidth())
                             .withMaximumNumColumns (1)
                             .withStandardItemHeight (npEsIPhone() ? 30 : 26),
            [sp] (int r)
            {
                if (sp == nullptr || r <= 0) return;
                const int i = r - 1;
                if (i < 0 || i >= sp->keys.size()) return;
                sp->selTonoIdx = i;   // solo selecciona; se aplica con el botón "Cargar"
                sp->repaint();
            });
    }

    void mouseDrag (const juce::MouseEvent& e) override   // scroll táctil (biblioteca o diálogo de tono)
    {
        if (tonoDragging)
        {
            const int dy = e.y - tonoDragStartY;
            if (std::abs (dy) > 4) tonoMoved = true;
            tonoScroll = juce::jlimit (0, tonoMaxScroll(), tonoScrollStart - dy);
            resized(); repaint();
            return;
        }
        if (! bibDragging) return;
        const int dy = e.y - bibDragStartY;
        if (std::abs (dy) > 4) bibMoved = true;
        bibScroll = juce::jlimit (0, bibMaxScroll(), bibScrollStart - dy);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override      // tap (sin arrastre) = elegir canción
    {
        const bool eraTonoDrag = tonoDragging && tonoMoved;
        tonoDragging = false;
        if (eraTonoDrag) return;   // fue un desplazamiento del diálogo, no un tap
        if (renderingSem != 99) { bibDragging = false; return; }
        if (mode == Biblioteca && bibDragging && ! bibMoved && bibListArea().contains (e.getPosition()))
            for (int i = 0; i < bib.size(); ++i)
                if (bibRect (i).contains (e.getPosition())) { if (onPickSong) onPickSong (bib[i].id); break; }
        bibDragging = false;
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        double d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        if (w.isReversed) d = -d;
        if (mode == Tono && tonoMaxScroll() > 0)
        {
            tonoScroll = juce::jlimit (0, tonoMaxScroll(), tonoScroll - (int) (d * 300.0));
            resized(); repaint();
            return;
        }
        if (mode != Biblioteca || ! bibListArea().contains (e.getPosition())) return;
        bibScroll = juce::jlimit (0, bibMaxScroll(), bibScroll + (int) (d * 300.0));
        repaint();
    }

    void startRender (int i)
    {
        renderingSem = keys[i].sem; pendIdx = i; progHechos = 0; progTotal = 0;
        prepFrame = 0;   // spinner "Preparando…" mientras se prepara
        const auto url = serverUrl, tok = token; const int sid = songId, sem = keys[i].sem;
        juce::Thread::launch ([url, tok, sid, sem]
        { httpPostForm (url + "/api/live/render/" + juce::String (sid) + "/" + juce::String (sem), {}, tok); });
        startTimerHz (2);
        repaint();
    }

    void timerCallback() override
    {
        if (renderingSem == 99) { stopTimer(); return; }
        ++prepFrame;   // anima el "Preparando…"
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

    std::function<void()>    onOpenMenu, onFaderArm, onPower;
    std::function<void(int)> onSel;    // -1 = Auto/Link ; 0..11 = tono

    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };

    PadPanel()
    {
        fader.setRange (-60.0, 0.0, 0.1);
        fader.setValue (0.0, juce::dontSendNotification);
        addAndMakeVisible (fader);
    }
    void setArmMode (bool on) { armMode = on; fader.setInterceptsMouseClicks (! on, ! on); repaint(); }

    juce::Rectangle<int> rPower, rLink, rPort, rName, rStatus, rFader, rFaderLbl;
    juce::Rectangle<int> rCell[12];
    int hoverCell = -1;   // almohadilla bajo el mouse (sombreado)

    void mouseMove (const juce::MouseEvent& e) override
    {
        int h = -1;
        for (int i = 0; i < 12; ++i) if (rCell[i].contains (e.getPosition())) { h = i; break; }
        if (h != hoverCell) { hoverCell = h; repaint(); }
    }
    void mouseExit (const juce::MouseEvent&) override { if (hoverCell != -1) { hoverCell = -1; repaint(); } }

    static const char* noteName (int i)
    {
        static const char* N[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return (i >= 0 && i < 12) ? N[i] : "";
    }

    // Área (en pantalla) de la barra selectora, para anclar ahí el menú de packs.
    juce::Rectangle<int> packBarScreenArea() const { return rName + getScreenPosition(); }

    void resized() override
    {
        const bool ph = npEsIPhone();
        auto full = getLocalBounds();
        auto fcol = full.removeFromRight (ph ? 58 : 74).reduced (0, ph ? 6 : 6);   // columna del fader casi a todo lo alto
        rFaderLbl = fcol.removeFromBottom (ph ? 14 : 14);
        rFader = fcol.reduced (ph ? 8 : 10, 0);            // fader lo más largo posible
        fader.setBounds (rFader);
        auto r = full.reduced (ph ? 8 : 14);
        r.removeFromRight (ph ? 8 : 12);

        auto top = r.removeFromTop (ph ? 38 : 40);     // barra selectora más baja (empuja los tonos arriba)
        rPower = top.removeFromLeft (ph ? 42 : 46).reduced (0, ph ? 5 : 5);   // ON/OFF del pad (antes estaba en el encabezado)
        top.removeFromLeft (ph ? 6 : 8);
        rLink = top.removeFromLeft (ph ? 46 : 44).reduced (0, ph ? 5 : 5);
        top.removeFromLeft (ph ? 8 : 10);
        rName = top.reduced (0, ph ? 4 : 5);           // la barra de vidrio ocupa el resto
        { auto inner = rName.reduced (5); rPort = inner.removeFromLeft (inner.getHeight()); }   // portada cuadrada DENTRO de la barra
        r.removeFromTop (ph ? 4 : 7);
        rStatus = r.removeFromTop (ph ? 13 : 16);
        r.removeFromTop (ph ? 5 : 8);

        // Grilla 6×2: celdas compactas, juntas y centradas (estilo igual, solo más chico).
        const int cols = 6, rows = 2, gap = ph ? 4 : 6;
        int cw = (r.getWidth() - (cols - 1) * gap) / cols;
        cw = juce::jmin (cw, ph ? 46 : 54);            // tope de ancho -> botones un pelín más chicos
        const int chh = ph ? 34 : 38;                 // alto fijo un pelín más chico
        const int gridW = cols * cw + (cols - 1) * gap;
        const int gridH = rows * chh + (rows - 1) * gap;
        const int ox = r.getX() + juce::jmax (0, (r.getWidth()  - gridW) / 2);
        const int oy = r.getY() + juce::jmax (0, (r.getHeight() - gridH) / 2);
        for (int i = 0; i < 12; ++i)
        {
            const int cx = i % cols, cy = i / cols;
            rCell[i] = juce::Rectangle<int> (ox + cx * (cw + gap),
                                             oy + cy * (chh + gap), cw, chh);
        }
    }

    void drawCell (juce::Graphics& g, juce::Rectangle<int> b, const juce::String& txt,
                   bool active, bool playing, bool ready, bool hover = false)
    {
        auto rf = b.toFloat();
        const float rad = 9.0f;
        juce::Colour bg = active ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff232323);
        if (hover) bg = bg.brighter (0.12f);     // sombreado al pasar el mouse
        if (! ready) bg = bg.withAlpha (0.30f);
        g.setColour (bg);
        g.fillRoundedRectangle (rf, rad);
        if (playing && ! active) { g.setColour (juce::Colour (0xff2E8BFF)); g.drawRoundedRectangle (rf.reduced (1.5f), rad, 2.0f); }
        g.setColour (active ? juce::Colours::black : juce::Colour (0xfff2f2f2));
        g.setFont (juce::Font ("Futura", juce::jlimit (16.0f, 24.0f, b.getHeight() * 0.52f), juce::Font::bold));   // fuente de los tonos (escala al alto)
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
        // Fondo con esquinas redondeadas + borde sutil (igual que el panel de faders).
        auto rf = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff0f0f0f));
        g.fillRoundedRectangle (rf, 12.0f);
        g.setColour (juce::Colour (0x22ffffff));
        g.drawRoundedRectangle (rf.reduced (0.5f), 12.0f, 1.0f);
        // PAD ON/OFF (antes estaba en el encabezado; prende/apaga el motor de pads)
        {
            g.setColour (enabled ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff232323));
            g.fillRoundedRectangle (rPower.toFloat(), 9.0f);
            g.setColour (enabled ? juce::Colours::black : juce::Colour (0xfff2f2f2));
            g.setFont (juce::Font (12.0f, juce::Font::bold));
            g.drawText ("PAD", rPower, juce::Justification::centred);
        }
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
            g.setColour (juce::Colour (0xff232323)); g.fillRoundedRectangle (rf, 8.0f);
            g.setColour (juce::Colour (0xff3a3a3a)); g.drawRoundedRectangle (rf.reduced (0.5f), 8.0f, 1.0f);
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
        if (done < 12)   // mientras baja se muestra el progreso; ya listos, sin texto
        {
            g.setColour (juce::Colour (0xffb0b0b0));
            g.setFont (juce::Font (12.0f));
            g.drawText ("Descargando " + juce::String (done) + "/12...", rStatus, juce::Justification::centredLeft);
        }
        // 12 tonos
        for (int i = 0; i < 12; ++i)
            drawCell (g, rCell[i], noteName (i), sel == i + 1, (sel == 0 && playingIdx == i), (readyMask & (1 << i)) != 0, hoverCell == i);
        // fader
        g.setColour (juce::Colour (0xffb0b0b0));
        g.setFont (juce::Font (11.0f));
        g.drawText ("PAD", rFaderLbl, juce::Justification::centred);
        // El marco azul de asignación MIDI del fader lo dibuja MainComponent con drawKeyBadge
        // (igual que el resto de los faders), para que se vea idéntico.
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto p = e.getPosition();
        if (armMode && rFader.contains (p)) { if (onFaderArm) onFaderArm(); return; }
        if (rPower.contains (p)) { if (onPower)   onPower();   return; }
        if (rLink.contains (p)) { if (onSel)      onSel (-1); return; }
        if (rName.contains (p)) { if (onOpenMenu) onOpenMenu(); return; }
        for (int i = 0; i < 12; ++i)
            if (rCell[i].contains (p)) { if ((readyMask & (1 << i)) && onSel) onSel (i); return; }
    }
};

// TextEditor que avisa cuando gana/pierde el foco (para subir la tarjeta y que el
// teclado en pantalla del iPad no tape el boton "Entrar").
struct FocusTextEditor : public juce::TextEditor
{
    std::function<void()> onFocus, onBlur;
    void focusGained (FocusChangeType t) override { juce::TextEditor::focusGained (t); if (onFocus) onFocus(); }
    void focusLost   (FocusChangeType t) override { juce::TextEditor::focusLost (t);   if (onBlur)  onBlur();  }
};

// ───────── Fuente de marca Space Grotesk (empaquetada) ─────────
// Se carga del binario para que se vea IDÉNTICA en iPhone, iPad y Mac
// (no depende de que el sistema tenga la fuente instalada).
static juce::Font npFontMarca (float alturaPx, bool bold = false)
{
    static juce::Typeface::Ptr reg = juce::Typeface::createSystemTypefaceFor (
        BinaryData::SpaceGroteskRegular_ttf, (size_t) BinaryData::SpaceGroteskRegular_ttfSize);
    static juce::Typeface::Ptr bld = juce::Typeface::createSystemTypefaceFor (
        BinaryData::SpaceGroteskBold_ttf,    (size_t) BinaryData::SpaceGroteskBold_ttfSize);
    juce::Font f (bold ? bld : reg);
    f.setHeight (alturaPx);
    return f;
}

// ───────── Panel de login para tactil (iOS/iPad): tarjeta centrada ─────────
// Reemplaza al AlertWindow (que en pantalla completa se estira feo) por una
// tarjeta propia con estilo NeuralPlay. En escritorio no se usa.
// Campos de texto con esquinas redondeadas y borde fino (para el login).
struct NPRoundFieldLnF : public juce::LookAndFeel_V4
{
    void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& te) override
    {
        g.setColour (te.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, 12.0f);
    }
    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& te) override
    {
        const bool foc = te.hasKeyboardFocus (true);
        g.setColour (te.findColour (foc ? juce::TextEditor::focusedOutlineColourId
                                        : juce::TextEditor::outlineColourId));
        g.drawRoundedRectangle (0.6f, 0.6f, (float) w - 1.2f, (float) h - 1.2f, 12.0f, foc ? 1.4f : 1.0f);
    }
    // Mismo tipo de letra (Space Grotesk) para el botón "Entrar".
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return npFontMarca (juce::jmin (17.0f, buttonHeight * 0.42f), true);
    }
};

// ── Estilo fino y muy redondeado para el AlertWindow de "Reportar un problema". ──
struct NPAlertLnF : public juce::LookAndFeel_V4
{
    void drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert,
                       const juce::Rectangle<int>& /*textArea*/, juce::TextLayout& textLayout) override
    {
        const float corner = 22.0f;
        auto full = alert.getLocalBounds().toFloat().reduced (1.0f);

        g.setColour (juce::Colour (0xff161616));                       // fondo oscuro de la app
        g.fillRoundedRectangle (full, corner);
        g.setColour (juce::Colour (0x1affffff));                       // borde muy fino y sutil
        g.drawRoundedRectangle (full.reduced (0.5f), corner, 1.0f);

        g.setColour (alert.findColour (juce::AlertWindow::textColourId));
        juce::Rectangle<int> txt (alert.getLocalBounds().getX() + 24, 28,
                                  alert.getLocalBounds().getWidth() - 48,
                                  alert.getLocalBounds().getHeight() - getAlertWindowButtonHeight() - 20);
        textLayout.draw (g, txt.toFloat());
    }
    int getAlertWindowButtonHeight() override { return 38; }

    // Botones estilo app: píldora redondeada. Azul = primario, rojo = destructivo (Borrar), gris = Cancelar.
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (4.0f, 5.0f);
        const float rad = juce::jmin (r.getHeight() * 0.5f, 11.0f);
        const auto t = b.getButtonText().toLowerCase();
        juce::Colour base;
        if (t.contains ("borrar") || t.contains ("eliminar")) base = juce::Colour (0xffCf4444);   // destructivo
        else if (t.contains ("cancelar"))                     base = juce::Colour (0xff2a2d33);    // cancelar
        else                                                  base = juce::Colour (0xff2E6BE6);    // primario
        if (down) base = base.darker (0.18f); else if (over) base = base.brighter (0.12f);
        g.setColour (base); g.fillRoundedRectangle (r, rad);
    }
    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (14.0f, juce::Font::bold));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, true);
    }
};

// Enlace pequeño "Enviar correo a soporte" (una sola línea, centrado) para el
// diálogo de reportar un problema.
struct NPSupportLink : public juce::Component
{
    std::function<void()> onClick;
    NPSupportLink() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff9aa0a6));
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.drawText (juce::String::fromUTF8 ("Enviar correo a soporte"),
                    getLocalBounds(), juce::Justification::centred);
    }
    void mouseUp (const juce::MouseEvent&) override { if (onClick) onClick(); }
};

// ── Diálogo estilo app (campos redondeados + calendario) para crear / duplicar repertorios.
//    Reemplaza al AlertWindow feo. Se usa solo en Mac por ahora. ──
struct NPDatePrompt : public juce::Component
{
    juce::String titulo, okText;
    juce::TextEditor nameField;
    juce::TextButton okBtn, cancelBtn, prevBtn, nextBtn;
    NPRoundFieldLnF fieldLnf;
    int calYear = 2025, calMonth = 0;                 // mes mostrado (0-11)
    int selYear = 2025, selMonth = 0, selDay = 1;     // fecha elegida
    std::function<void (juce::String, juce::String)> onOk;

    NPDatePrompt()
    {
        nameField.setColour (juce::TextEditor::backgroundColourId,       juce::Colour (0xff202227));
        nameField.setColour (juce::TextEditor::textColourId,             juce::Colour (0xfff2f2f2));
        nameField.setColour (juce::TextEditor::outlineColourId,          juce::Colour (0xff3a3d44));
        nameField.setColour (juce::TextEditor::focusedOutlineColourId,   juce::Colour (0xff8a94a6));
        nameField.setLookAndFeel (&fieldLnf);
        nameField.setFont (juce::Font (16.0f));
        nameField.setJustification (juce::Justification::centredLeft);   // texto centrado verticalmente
        nameField.setIndents (12, 0);
        nameField.onReturnKey = [this] { grabKeyboardFocus(); };          // táctil: Return cierra el teclado
        addAndMakeVisible (nameField);
        setWantsKeyboardFocus (true);

        okBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xfff2f2f2));
        okBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff0a0a0a));
        okBtn.onClick = [this] { confirm(); };
        addAndMakeVisible (okBtn);
        cancelBtn.setButtonText ("Cancelar");
        cancelBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        cancelBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        cancelBtn.onClick = [this] { setVisible (false); };
        addAndMakeVisible (cancelBtn);
        prevBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x80\xb9"));
        prevBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        prevBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        prevBtn.onClick = [this] { if (--calMonth < 0) { calMonth = 11; --calYear; } repaint(); };
        addAndMakeVisible (prevBtn);
        nextBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x80\xba"));
        nextBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        nextBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xfff2f2f2));
        nextBtn.onClick = [this] { if (++calMonth > 11) { calMonth = 0; ++calYear; } repaint(); };
        addAndMakeVisible (nextBtn);
        setAlwaysOnTop (true);
    }
    ~NPDatePrompt() override { nameField.setLookAndFeel (nullptr); }

    void abrir (juce::String t, juce::String ok, juce::String nombre)
    {
        titulo = t; okText = ok; okBtn.setButtonText (ok);
        nameField.setText (nombre, juce::dontSendNotification);
        const auto now = juce::Time::getCurrentTime();
        selYear = calYear = now.getYear();
        selMonth = calMonth = now.getMonth();   // 0-11
        selDay = now.getDayOfMonth();
        setVisible (true); toFront (true);
        resized(); repaint();
       #if ! JUCE_IOS
        nameField.grabKeyboardFocus();   // en táctil NO enfocamos: el teclado taparía el calendario/Crear
       #endif
    }

    static int diasEnMes (int y, int m)
    {
        static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
        if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
        return d[m];
    }
    int primerDiaCol (int y, int m) const           // columna (0=Lun..6=Dom) del día 1
    { juce::Time t (y, m, 1, 0, 0); return (t.getDayOfWeek() + 6) % 7; }
    juce::String fechaStr() const
    { return juce::String::formatted ("%04d-%02d-%02d", selYear, selMonth + 1, selDay); }

    struct Metrics { int cardW, cardH, pad, titleY, nombreY, fieldY, fieldH, fechaY, gridTop, gridH, btnY, btnH; };
    Metrics M() const
    {   //                cardW cardH pad titY nomY fldY fldH fecY gTop gH  btnY btnH
        if (npEsIPhone()) return { 400, 396, 18, 12, 40, 56, 34, 98, 156, 168, 344, 34 };
        return              { 420, 500, 24, 20, 58, 78, 40, 126, 200, 240, 448, 40 };
    }
    juce::Rectangle<int> cardBounds() const { auto m = M(); return getLocalBounds().withSizeKeepingCentre (m.cardW, m.cardH); }
    juce::Rectangle<int> gridArea() const
    { auto m = M(); auto c = cardBounds().reduced (m.pad, 0); return { c.getX(), cardBounds().getY() + m.gridTop, c.getWidth(), m.gridH }; }
    juce::Rectangle<int> cellRect (int col, int rowi) const
    {
        auto g = gridArea(); const int cw = g.getWidth() / 7, hdr = 24, ch = (g.getHeight() - hdr) / 6;
        return { g.getX() + col * cw, g.getY() + hdr + rowi * ch, cw, ch };
    }

    void confirm()
    {
        auto n = nameField.getText().trim();
        setVisible (false);
        if (onOk) onOk (n, fechaStr());
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xC0000000));
        auto c = cardBounds().toFloat();
        g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (c, 16.0f);
        g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (c, 16.0f, 1.2f);
        auto m = M();
        auto in = cardBounds().reduced (m.pad, 0);
        g.setColour (juce::Colours::white); g.setFont (juce::Font (18.0f, juce::Font::bold));
        g.drawText (titulo, juce::Rectangle<int> (in.getX(), cardBounds().getY() + m.titleY, in.getWidth(), 26), juce::Justification::centred);
        g.setColour (juce::Colour (0xff9aa0a6)); g.setFont (juce::Font (12.5f));
        g.drawText ("Nombre", juce::Rectangle<int> (in.getX(), cardBounds().getY() + m.nombreY, in.getWidth(), 16), juce::Justification::centredLeft);
        g.drawText ("Fecha",  juce::Rectangle<int> (in.getX(), cardBounds().getY() + m.fechaY, in.getWidth(), 16), juce::Justification::centredLeft);
        g.setColour (juce::Colours::white); g.setFont (juce::Font (13.5f, juce::Font::bold));
        g.drawText (fechaStr(), juce::Rectangle<int> (in.getX(), cardBounds().getY() + m.fechaY - 2, in.getWidth(), 18), juce::Justification::centredRight);

        static const char* mesN[12] = { "Enero","Febrero","Marzo","Abril","Mayo","Junio","Julio","Agosto","Septiembre","Octubre","Noviembre","Diciembre" };
        auto g0 = gridArea();
        g.setColour (juce::Colours::white); g.setFont (juce::Font (14.0f, juce::Font::bold));
        g.drawText (juce::String (mesN[calMonth]) + " " + juce::String (calYear),
                    juce::Rectangle<int> (g0.getX(), g0.getY() - 30, g0.getWidth(), 24), juce::Justification::centred);
        static const char* dn[7] = { "L","M","M","J","V","S","D" };
        g.setColour (juce::Colour (0xff8a8a8a)); g.setFont (juce::Font (11.5f, juce::Font::bold));
        const int cw = g0.getWidth() / 7;
        for (int i = 0; i < 7; ++i)
            g.drawText (dn[i], juce::Rectangle<int> (g0.getX() + i * cw, g0.getY(), cw, 24), juce::Justification::centred);
        const int dim = diasEnMes (calYear, calMonth), off = primerDiaCol (calYear, calMonth);
        for (int d = 1; d <= dim; ++d)
        {
            const int idx = off + d - 1;
            auto cr = cellRect (idx % 7, idx / 7).reduced (3);
            const bool sel = (calYear == selYear && calMonth == selMonth && d == selDay);
            if (sel) { g.setColour (juce::Colour (0xff2E6BE6)); g.fillRoundedRectangle (cr.toFloat(), 8.0f); }
            g.setColour (sel ? juce::Colours::white : juce::Colour (0xffdedede));
            g.setFont (juce::Font (13.0f, sel ? juce::Font::bold : juce::Font::plain));
            g.drawText (juce::String (d), cr, juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto m = M();
        auto in = cardBounds().reduced (m.pad, 0);
        nameField.setBounds (juce::Rectangle<int> (in.getX(), cardBounds().getY() + m.fieldY, in.getWidth(), m.fieldH));
        auto g0 = gridArea();
        prevBtn.setBounds (g0.getX(), g0.getY() - 32, 34, 26);
        nextBtn.setBounds (g0.getRight() - 34, g0.getY() - 32, 34, 26);
        const int half = in.getWidth() / 2 - 6;
        cancelBtn.setBounds (in.getX(), cardBounds().getY() + m.btnY, half, m.btnH);
        okBtn.setBounds     (in.getRight() - half, cardBounds().getY() + m.btnY, half, m.btnH);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int dim = diasEnMes (calYear, calMonth), off = primerDiaCol (calYear, calMonth);
        for (int d = 1; d <= dim; ++d)
        {
            const int idx = off + d - 1;
            if (cellRect (idx % 7, idx / 7).contains (e.getPosition()))
            { selDay = d; selMonth = calMonth; selYear = calYear; repaint(); return; }
        }
        if (! cardBounds().contains (e.getPosition())) setVisible (false);
    }
};

class NeuralLoginOverlay : public juce::Component
{
public:
    std::function<void (juce::String, juce::String)> onSubmit;

    ~NeuralLoginOverlay() override
    {
        email.setLookAndFeel (nullptr);   // soltar el LnF antes de que se destruya
        pass.setLookAndFeel (nullptr);
        entrar.setLookAndFeel (nullptr);
    }

    NeuralLoginOverlay()
    {
        setInterceptsMouseClicks (true, true);
        setWantsKeyboardFocus (true);   // para poder quitar el foco de los campos (ocultar el teclado)

        title.setText (juce::String::fromUTF8 ("NeuralPlay"), juce::dontSendNotification);
        title.setJustificationType (juce::Justification::centred);
        title.setColour (juce::Label::textColourId, juce::Colour (0xfff2f2f2));
        title.setFont (juce::Font (30.0f, juce::Font::bold));
        addAndMakeVisible (title);

        subtitle.setText (juce::String::fromUTF8 ("Inicia sesi\xc3\xb3n con tus credenciales de administrador"),
                          juce::dontSendNotification);
        subtitle.setJustificationType (juce::Justification::centred);
        subtitle.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0a6));
        subtitle.setFont (juce::Font (13.5f));
        addAndMakeVisible (subtitle);

        auto styleField = [this] (juce::TextEditor& t, const juce::String& ph, bool pass)
        {
            t.setColour (juce::TextEditor::backgroundColourId,       juce::Colour (0xff202227));
            t.setColour (juce::TextEditor::textColourId,            juce::Colour (0xfff2f2f2));
            t.setColour (juce::TextEditor::outlineColourId,         juce::Colour (0xff3a3d44));
            t.setColour (juce::TextEditor::focusedOutlineColourId,  juce::Colour (0xff8a94a6));
            t.setColour (juce::CaretComponent::caretColourId,       juce::Colour (0xfff2f2f2));
            t.setLookAndFeel (&fieldLnf);                            // esquinas redondeadas + borde fino
            t.setFont (npFontMarca (16.0f, false));
            t.setTextToShowWhenEmpty (ph, juce::Colour (0xff6b7280));
            t.setJustification (juce::Justification::centred);       // texto centrado en la cajita
            t.setIndents (10, 0);
            if (pass) t.setPasswordCharacter ((juce_wchar) 0x2022);
        };
        styleField (email, juce::String::fromUTF8 ("Email"), false);
        email.onReturnKey = [this] { pass.grabKeyboardFocus(); };   // Enter → salta a Contraseña
        addAndMakeVisible (email);
        styleField (pass, juce::String::fromUTF8 ("Contrase\xc3\xb1" "a"), true);
        pass.onReturnKey = [this] { submit(); };                    // Enter en Contraseña → Entrar
        addAndMakeVisible (pass);

        // Subir la tarjeta cuando cualquiera de los campos toma foco (teclado en pantalla),
        // y devolverla al centro cuando ninguno queda enfocado.
        auto onFocus = [this] { if (! kbShown) { kbShown = true; resized(); repaint(); } };
        auto onBlur  = [this]
        {
            juce::Component::SafePointer<NeuralLoginOverlay> sp (this);
            juce::MessageManager::callAsync ([sp]
            {
                if (sp == nullptr) return;
                const bool anyFocus = sp->email.hasKeyboardFocus (true) || sp->pass.hasKeyboardFocus (true);
                if (! anyFocus && sp->kbShown) { sp->kbShown = false; sp->resized(); sp->repaint(); }
            });
        };
       #if JUCE_IOS || JUCE_ANDROID
        email.onFocus = onFocus; email.onBlur = onBlur;
        pass.onFocus  = onFocus; pass.onBlur  = onBlur;
       #else
        juce::ignoreUnused (onFocus, onBlur);
       #endif

        entrar.setButtonText (juce::String::fromUTF8 ("Entrar"));
        entrar.setColour (juce::TextButton::buttonColourId,    juce::Colour (0xfff2f2f2));
        entrar.setColour (juce::TextButton::textColourOffId,   juce::Colour (0xff0a0a0a));
        entrar.setLookAndFeel (&fieldLnf);                      // fuente Avenir Next en el botón
        entrar.onClick = [this] { submit(); };
        addAndMakeVisible (entrar);

        // "Recordar mi contrase\xc3\xb1" a": guarda correo+contrase\xc3\xb1" a en el Keychain del sistema.
        recordar.setButtonText (juce::String::fromUTF8 ("Recordar mi contrase\xc3\xb1" "a"));
        recordar.setColour (juce::ToggleButton::textColourId,   juce::Colour (0xff9aa0a6));
        recordar.setColour (juce::ToggleButton::tickColourId,   juce::Colour (0xfff2f2f2));
        recordar.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colour (0xff3a3d44));
        addAndMakeVisible (recordar);

        error.setJustificationType (juce::Justification::centred);
        error.setColour (juce::Label::textColourId, juce::Colour (0xffef6b6b));
        error.setFont (juce::Font (14.0f));
        addAndMakeVisible (error);
    }

    void setLogo (juce::Image i) { logo = i; repaint(); }

    // Prellena los campos con credenciales recordadas (y marca el checkbox).
    void prefill (const juce::String& correo, const juce::String& clave, bool recordarOn)
    {
        email.setText (correo, juce::dontSendNotification);
        pass.setText  (clave,  juce::dontSendNotification);
        recordar.setToggleState (recordarOn, juce::dontSendNotification);
    }

    bool quiereRecordar() const { return recordar.getToggleState(); }

    void showError (const juce::String& m)
    {
        error.setText (m, juce::dontSendNotification);
        setBusy (false);
        resized();   // re-ajustar: el mensaje puede ocupar varias líneas
        repaint();
    }

    void setBusy (bool b)
    {
        entrar.setEnabled (! b);
        entrar.setButtonText (b ? juce::String::fromUTF8 ("Entrando\xe2\x80\xa6")
                                : juce::String::fromUTF8 ("Entrar"));
    }

    void reset()
    {
        error.setText ({}, juce::dontSendNotification);
        setBusy (false);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0a0a));
        g.setColour (juce::Colour (0xff141518));
        g.fillRoundedRectangle (card.toFloat(), 18.0f);
        g.setColour (juce::Colour (0xff2a2c31));
        g.drawRoundedRectangle (card.toFloat(), 18.0f, 1.0f);
        if (logo.isValid())
            g.drawImageWithin (logo, logoBounds.getX(), logoBounds.getY(),
                               logoBounds.getWidth(), logoBounds.getHeight(),
                               juce::RectanglePlacement::centred);
    }

    void resized() override
    {
        auto b = getLocalBounds();

        // ── Tarjeta de login IDÉNTICA en iPhone, iPad y Mac ──
        //    Un solo layout (mismas medidas absolutas) + fuente fija "Avenir Next"
        //    (existe igual en iOS y macOS → renderiza idéntico en las 3).
        //    En táctil, si el teclado sube, el formulario se pega arriba y se
        //    oculta el logo/subtítulo para ganar espacio.
        auto safe = b;
       #if JUCE_IOS || JUCE_ANDROID
        if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            safe = d->safeAreaInsets.subtractedFrom (b);
       #endif
        const bool brand = ! kbShown;

        // Space Grotesk empaquetada: idéntica en las 3 plataformas.
        // "NeuralPlay" en Bold 700, la descripción en Regular 400.
        title.setFont    (npFontMarca (26.0f, true));
        subtitle.setFont (npFontMarca (15.0f, false));

        const int cardW = juce::jmin (430, b.getWidth() - 40);
        // El error puede ocupar varias líneas (p.ej. el aviso de sesión única) → alto dinámico.
        const int hErr = error.getText().isNotEmpty() ? 54 : 0;
        const int hLogo = 60, hTitle = 34, hSub = 20, hField = 46, hBtn = 48, hTog = 28;
        int cardH = 14 + hField + 11 + hField + 8 + hTog + 8 + hErr + 8 + hBtn + 14;   // form + paddings
        if (brand) cardH += hLogo + 2 + hTitle + 0 + hSub + 16;
        int top = kbShown ? (safe.getY() + 8)
                          : juce::jmax (safe.getY() + 8, b.getCentreY() - cardH / 2);
        card = juce::Rectangle<int> (b.getCentreX() - cardW / 2, top, cardW, cardH);
        auto in = card.reduced (18, 14);
        if (brand)
        {
            logoBounds = in.removeFromTop (hLogo); in.removeFromTop (2);
            title.setBounds    (in.removeFromTop (hTitle)); in.removeFromTop (0);
            subtitle.setBounds (in.removeFromTop (hSub));   in.removeFromTop (16);
        }
        else { logoBounds = {}; title.setBounds ({}); subtitle.setBounds ({}); }
        title.setVisible (brand); subtitle.setVisible (brand);
        email.setBounds  (in.removeFromTop (hField)); in.removeFromTop (11);
        pass.setBounds   (in.removeFromTop (hField)); in.removeFromTop (8);
        recordar.setBounds (in.removeFromTop (hTog)); in.removeFromTop (8);
        error.setBounds  (in.removeFromTop (hErr));   in.removeFromTop (8);
        entrar.setBounds (in.removeFromTop (hBtn));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Tocar fuera de los campos oculta el teclado (iPhone/iPad).
        if (! email.getBounds().contains (e.getPosition())
            && ! pass.getBounds().contains (e.getPosition()))
            grabKeyboardFocus();   // el overlay toma el foco → los campos lo pierden → se cierra el teclado
    }

private:
    void submit()
    {
        error.setText ({}, juce::dontSendNotification);
        if (onSubmit) { setBusy (true); onSubmit (email.getText().trim(), pass.getText()); }
    }

    NPRoundFieldLnF fieldLnf;   // antes de los campos: se destruye después de ellos
    juce::Label title, subtitle, error;
    FocusTextEditor email, pass;
    juce::ToggleButton recordar;
    juce::TextButton entrar;
    juce::Image logo;
    juce::Rectangle<int> card, logoBounds;
    bool kbShown = false;   // teclado en pantalla visible → subir la tarjeta
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeuralLoginOverlay)
};

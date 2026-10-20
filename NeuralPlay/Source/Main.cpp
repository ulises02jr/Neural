#include <JuceHeader.h>
#include "BinaryData.h"
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <array>
#if JUCE_IOS || JUCE_MAC
 #include <ifaddrs.h>
 #include <net/if.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <cstring>
#endif

#include "np_util.h"
#include "np_net.h"

#include "np_ui.h"

class MainComponent : public juce::AudioAppComponent,
                      private juce::Timer,
                      private juce::ChangeListener,
                      private juce::MidiInputCallback
{
public:
    MainComponent()
    {
        // Sesión única por dispositivo: si el servidor cierra esta sesión (se abrió
        // en otro dispositivo), olvidamos el token y mostramos el login con aviso.
        {
            juce::Component::SafePointer<MainComponent> selfKick (this);
            npOnSessionKicked = [selfKick] (juce::String msg)
            {
                if (selfKick == nullptr) return;
                selfKick->serverToken.clear();
                selfKick->serverSession.clear();
                npSessionToken.clear();
                selfKick->guardarConfigCuenta();
                selfKick->connStatus.setText (msg, juce::dontSendNotification);
                selfKick->mostrarLoginDialog();
                if (selfKick->loginOverlay != nullptr) selfKick->loginOverlay->showError (msg);
            };
        }
        loadConfig();
        // Sin sesión: el login se muestra DESPUÉS del splash (~2s), no de inmediato,
        // para que el logo de inicio se alcance a ver. Se dispara desde timerCallback.
        setLookAndFeel (&pillLnf);
        juce::LookAndFeel::setDefaultLookAndFeel (&pillLnf);   // Space Grotesk también en menús/popups/diálogos
        setWantsKeyboardFocus (true);   // #4 recibir teclas para el mapping de teclado
        logoImg = juce::ImageFileFormat::loadFrom (BinaryData::AppIcon_png, (size_t) BinaryData::AppIcon_pngSize);
        // Logo interno (wordmark "NeuralPlay") SOLO para la esquina del header.
        // El icono de la app (AppIcon) se mantiene en splash/login y como icono general.
        logoInternoImg = juce::ImageFileFormat::loadFrom (BinaryData::LogoInterno_png, (size_t) BinaryData::LogoInterno_pngSize);
        splash.logo = logoInternoImg;   // pantalla de inicio: mostrar el wordmark "NeuralPlay", no el icono cuadrado
        formatManager.registerBasicFormats();
       #if JUCE_MAC || JUCE_IOS
        // CoreAudio decodifica los formatos comprimidos (MP3/AAC/M4A) ademas de WAV/AIFF.
        // Se registra tambien en iOS para reproducir los stems ORIGINALES tal como el
        // usuario los subio (si subio WAV suena WAV; si subio MP3 suena MP3). NeuralPlay
        // nunca fuerza un formato: baja el archivo original que lista el servidor.
        formatManager.registerFormat (new juce::CoreAudioFormat(), false);
       #endif
        readThread.startThread (juce::Thread::Priority::high);   // alimenta los buffers a tiempo (evita cortes con muchos stems)
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
        repPicker.onSaveAs = [this] { saveRepertoireAsNew(); };
        repPicker.onDownload = [this] (juce::String id) { downloadRepertoireOffline (id); };
        addChildComponent (repPicker);
        addChildComponent (datePrompt);

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
        settingsPanel.onReportar = [this] { settingsPanel.setVisible (false); reportarProblema(); };
        settingsPanel.onLogout  = [this] { settingsPanel.setVisible (false); cerrarSesion(); };
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
        loadArr();
        loadGuiaSel();
        loadTrans();
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
        storagePanel.onBack       = [this]
        {
            settingsPanel.setState (syncEnabled, syncLinked.load());
            settingsPanel.setBounds (getLocalBounds());
            settingsPanel.setVisible (true);
            settingsPanel.toFront (true);
        };
        addChildComponent (storagePanel);

        audioCfg.onDevice = [this] (const juce::String& d) { applyAudioDevice (d); };
        audioCfg.onRoute  = [this] (int f, int m, int b)   { setFamRoute (f, m, b); };
        audioCfg.onAutoPan = [this] (bool on) { autoPan.store (on); saveAudioRouting(); };
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
            // Arreglo activo (secuencia de índices de sección, sin Conteo). Vacío = canción completa.
            // 'step' se reindexa a la lista FILTRADA (arrStr) para que concuerde con el chart.
            juce::String arrStr; int stepNow = -1;
            {
                const juce::ScopedLock sl (graphLock);
                if (arrangementActive.load() && arrangement.size() > 0)
                {
                    const int rawStep = arrStep.load();
                    bool first = true; int filtPos = 0;
                    for (int i = 0; i < arrangement.size(); ++i)
                    {
                        const int s = arrangement[i];
                        if (s < 0) continue;   // omitir Conteo (kConteoIdx) en el chart
                        if (! first) arrStr << ",";
                        arrStr << s; first = false;
                        if (i <= rawStep) stepNow = filtPos;   // sección real actual en la lista filtrada
                        ++filtPos;
                    }
                    if (stepNow < 0 && filtPos > 0) stepNow = 0;   // aún en el Conteo: resaltar la 1ª
                }
            }
            juce::String j;
            j << "{\"idx\":" << liveSectionIdx.load()
              << ",\"ver\":" << liveSongVer.load()
              << ",\"step\":" << stepNow
              << ",\"arr\":[" << arrStr << "]"
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

        saveArrBtn.setButtonText (juce::String::fromUTF8 ("Guardar arreglo"));   // Fase A: aplica el arreglo editado
        saveArrBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff14301a));
        saveArrBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff8fe0a0));
        saveArrBtn.onClick = [this] { guardarArreglo(); };
        addChildComponent (saveArrBtn);   // se muestra solo en la barra de Editar

        saveAsNewBtn.setButtonText (juce::String::fromUTF8 ("Guardar como nuevo"));
        saveAsNewBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f1f1f));
        saveAsNewBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff8fe0a0));
        saveAsNewBtn.onClick = [this] { guardarArregloComoNuevo(); };
        addChildComponent (saveAsNewBtn);

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
        // Nuevo flujo: al elegir tono se cierra la ventana y la barra de "preparando"
        // aparece en la portada de la canción (como al cargar).
        repEdit.onElegir = [this] (int sem, juce::String nombre, bool yaListo)
        { const int sid = repEdit.songId; const bool add = repEdit.addFlow;
          const juce::String arr = repEdit.selectedArrName();
          // ¿cambió el tono? (solo canciones ya en el repertorio). Si NO cambió, no recargamos
          // el setlist (esa recarga reiniciaba a la canción 1); solo aplicamos el arreglo.
          int curSem = 0; bool found = false;
          if (! add)
              for (int i = 0; i < repertoire.size(); ++i)
                  if (repertoire.getReference (i).id == sid)
                  { curSem = repertoire.getReference (i).tono; found = true; break; }
          const bool tonoCambia = add || ! found || sem != curSem;
          // Guía elegida en el diálogo (preferencia para Original; con arreglo se fuerza Neural igual)
          songGuia[sid] = (repEdit.guiaMode == 1) ? 1 : 0;
          if (repEdit.repTrack) songRepTrack.insert (sid); else songRepTrack.erase (sid);   // permitir repetir/∞ en Track
          saveGuiaSel();
          if (tonoCambia) { pendingArrName = arr; prepararTono (sem, nombre, yaListo); }  // recarga: loadSong aplica el arreglo
          else            { pendingArrName.clear(); }                                     // sin recarga: no dejar pendiente
          aplicarArregloASong (sid, arr);        // aplica/guarda el arreglo (en vivo si es la canción actual)
          aplicarGuiaSegunEstado(); };           // dejar la guía (Tracks/Neural) acorde al estado
        repEdit.onDeleteArr = [this] (int sid, juce::String nombre) { borrarArreglo (sid, nombre); };
        repEdit.onRenameArr = [this] (int sid, juce::String nombre) { pedirRenombrarArreglo (sid, nombre); };
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

        // returnButton ahora es un icono vectorial (SkipStartButton), no texto.
        returnButton.onClick = [this] {
            if (clickOrArm (kaReturn)) return;
            irAlInicio();
        };
        addAndMakeVisible (returnButton);

        barNextBtn.pointsRight = true;                 // flecha derecha; barPrevBtn apunta a la izquierda
        for (auto* b : { &barPrevBtn, &barNextBtn })   // navegación por compás sobre el mapa
            addAndMakeVisible (b);
        barPrevBtn.onClick = [this] { if (clickOrArm (kaPrevBar)) return; seekSection (-1); };
        barNextBtn.onClick = [this] { if (clickOrArm (kaNextBar)) return; seekSection (+1); };

        fadeButton.onClick = [this] { if (clickOrArm (kaFade)) return; toggleFade(); };
        addAndMakeVisible (fadeButton);

        // transBtn ahora es SkipNextButton (ícono "siguiente" dibujado como vector),
        // idéntico en Mac e iOS. Antes usaba el glyph ⏭ que iOS mostraba como emoji/“?”.
        transBtn.onClick = [this]
        {
            const int at = autoTrans.load();
            juce::PopupMenu m;
            m.addSectionHeader (juce::String::fromUTF8 ("Ahora (al instante)"));
            m.addItem (1, juce::String::fromUTF8 ("Autolink (corte seco)"));
            m.addItem (2, juce::String::fromUTF8 ("Sobreponer (crossfade 2 compases)"));
            m.addSeparator();
            m.addSectionHeader (juce::String::fromUTF8 ("Al terminar la canci\xc3\xb3n (autom\xc3\xa1tico)"));
            m.addItem (3, juce::String::fromUTF8 ("Autolink al terminar"), true, at == 1);
            m.addItem (4, juce::String::fromUTF8 ("Sobreponer al terminar"), true, at == 2);
            m.addItem (5, juce::String::fromUTF8 ("Desactivar autom\xc3\xa1tico"), at != 0, false);
            juce::Component::SafePointer<MainComponent> sp (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&transBtn),
                [sp] (int r)
                {
                    if (sp == nullptr || r <= 0) return;
                    if      (r == 1) sp->transicionSiguiente (false);
                    else if (r == 2) sp->transicionSiguiente (true);
                    else if (r == 3) sp->setTransActual (1);   // guarda por repertorio + canción
                    else if (r == 4) sp->setTransActual (2);
                    else if (r == 5) sp->setTransActual (0);
                    sp->repaint();
                });
        };
        addChildComponent (transBtn);

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
        padPlayerBtn.onClick = [this] { if (clickOrArm (kaPadPlayer)) return; if (npEsIPhone()) phoneShowMap = false; setFaderView (3); };
        faderViewBtn.kind = 0; repeatBtn.kind = 1; infiniteBtn.kind = 2;
        faderViewBtn.active = true;
        addAndMakeVisible (faderViewBtn);
        addAndMakeVisible (repeatBtn);
        addAndMakeVisible (infiniteBtn);
        // En iPhone el botón de Faders alterna entre Faders y Mapa (escenario único).
        faderViewBtn.onClick = [this]
        {
            if (clickOrArm (kaFaderView)) return;
            if (npEsIPhone())
            {
                phoneShowMap = ! phoneShowMap;
                if (! phoneShowMap) { setFaderView (0); }
                else { updateFaderVisibility(); resized(); repaint(); }
                return;
            }
            setFaderView (0);
        };
        busesBtn.onClick     = [this] { if (clickOrArm (kaBuses))     return; if (npEsIPhone()) phoneShowMap = false; setFaderView (1); };
        muteMidiBtn.onClick  = [this] { if (! featMidi) { avisoPlanMidi(); return; } if (clickOrArm (kaMidi))      return; if (npEsIPhone()) phoneShowMap = false; setFaderView (2); };

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
       #if JUCE_IOS
        setup.bufferSize = 512;   // iPad: 512 = misma latencia que el Mac. Con 1024 el conteo y los saltos
                                   // se desfasaban (ventana de ~23 ms). Si reaparecen cortes con canciones muy
                                   // pesadas, subir a 640/768 (no a 1024).
       #else
        setup.bufferSize = 512;   // escritorio: buen balance latencia/estabilidad
       #endif
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
        splash.setAlwaysOnTop (true);   // cubre TODO durante los ~2s (que no se asomen las tarjetas)
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
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);   // soltar el default global antes de destruir pillLnf
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
        {
            const juce::ScopedLock tl (tailLock);   // Fase D: cola del crossfade
            tailTemp.setSize (2, juce::jmax (spb, 2048) + 8);
            for (int i = 0; i < resamplers2.size(); ++i)
                if (i < fileRates2.size()) { resamplers2[i]->setResamplingRatio (fileRates2[i] / sampleRate); resamplers2[i]->prepareToPlay (spb, sampleRate); }
        }
        prepared.store (true);
    }
    void releaseResources() override
    {
        prepared.store (false);
        for (auto* r : resamplers) r->releaseResources();
        { const juce::ScopedLock pl (padLock); for (auto* pv : padVoices) if (pv->res) pv->res->releaseResources(); }
        { const juce::ScopedLock tl (tailLock); for (auto* r : resamplers2) r->releaseResources(); }
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        renderTracks (info);   // mezcla de la canción (puede salir temprano y dejar el buffer en silencio)
        mixTail (info);        // Fase D: cola de la canción saliente (crossfade), independiente del graphLock
        mixPad (info);         // el pad SIEMPRE se mezcla encima (suena aunque no haya canción o esté en pausa)
    }

    // Fase D — suma la "cola" de la canción saliente con fade-out (crossfade Sobreponer).
    void mixTail (const juce::AudioSourceChannelInfo& info)
    {
        if (! tailActive.load()) return;
        const juce::ScopedTryLock tl (tailLock);
        if (! tl.isLocked() || resamplers2.isEmpty()) return;
        const int nn = info.numSamples;
        if (tailTemp.getNumSamples() < nn) return;   // dimensionado en prepareToPlay
        auto* b = info.buffer;
        const int useCh = juce::jmin (b->getNumChannels(), 32);
        if (useCh < 1) return;
        const double step = (double) nn / juce::jmax (1.0, deviceSampleRate * juce::jmax (0.2, tailFadeSec));
        const float gStart = (float) tailGainCur;
        const float gEnd   = (float) juce::jmax (0.0, tailGainCur - step);
        const float mg = masterGain.load();
        for (int t = 0; t < resamplers2.size(); ++t)
        {
            if (t < kMaxTracks && tailIsClick[t]) continue;   // no sonar el click de la saliente (evita choque con el click nuevo)
            tailTemp.clear();
            juce::AudioSourceChannelInfo ti (&tailTemp, 0, nn);
            resamplers2.getUnchecked (t)->getNextAudioBlock (ti);
            const float* sL = tailTemp.getReadPointer (0);
            const float* sR = tailTemp.getNumChannels() > 1 ? tailTemp.getReadPointer (1) : sL;
            float* oL = b->getWritePointer (0, info.startSample);
            float* oR = useCh > 1 ? b->getWritePointer (1, info.startSample) : nullptr;
            for (int n = 0; n < nn; ++n)
            {
                const float g = (gStart + (gEnd - gStart) * (n / (float) nn)) * mg;
                oL[n] += sL[n] * g;
                if (oR) oR[n] += sR[n] * g;
            }
        }
        tailGainCur = gEnd;
        if (tailGainCur <= 0.0005) tailActive.store (false);   // terminó el fade; se limpia en la próxima transición
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
        voice->buf.reset (new juce::BufferingAudioSource (rs, readThread, false, 240000, 2));
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
            // Guía dinámica: al saltar, cortar el anuncio en curso y reprogramar según la nueva posición.
            cueActiveBuf = -1; cuePos = 0; cuePendingIdx.store (-1);
            cueSchedStep = -1000; linSchedSec = -1000;
            cntIdx = cntN;   // cancelar el conteo hablado restante tras un salto
            // Salto inmediato del usuario o repetición: anunciar la sección en la que caímos.
            if (cueAnnounceLanded.exchange (false) && guideOn.load() && playing.load())
            {
                const double ps = (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate);
                int landSec = -999;
                if (arrangementActive.load() && arrangement.size() > 0)
                {
                    const int st = arrStep.load();
                    landSec = (st >= 0 && st < arrangement.size()) ? arrangement[st] : -999;
                    if (landSec < 0) landSec = liveSectionIndexAt (ps);   // conteo u otros → por posición
                }
                else landSec = liveSectionIndexAt (ps);
                if (landSec >= 0)
                {
                    const int clip = guideClipForSection (landSec);
                    if (clip >= 0) { cueActiveBuf = clip; cuePos = 0; }
                }
            }
            else cueAnnounceLanded.store (false);
        }
        if (! playing.load()) { guideWasPlaying = false; return; }   // pausa: re-armar guía al reanudar

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

        float afG = 1.0f;   // fundido de entrada tras un salto de arreglo no contiguo (anti-click)
        {
            const double fs = arrFadeStart.load(), fe = arrFadeEnd.load();
            if (fs >= 0.0 && fe > fs)
            {
                const double p = (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate);
                if (p >= fe || p < fs - 0.5) arrFadeStart.store (-1.0);          // terminó o saltamos lejos
                else if (p >= fs)            afG = (float) juce::jlimit (0.0, 1.0, (p - fs) / (fe - fs));
            }
        }

        for (int t = 0; t < resamplers.size(); ++t)
        {
            temp.clear();
            juce::AudioSourceChannelInfo ti (&temp, 0, nn);
            resamplers.getUnchecked (t)->getNextAudioBlock (ti);
            // Con guía Neural activa, la pista "Guía" del usuario se silencia (solo suena la voz).
            const bool guiaSilenciada = guideOn.load() && t < kMaxTracks && trackIsGuia[t];
            const bool audible = ! guiaSilenciada && ! trackMuted[t].load() && (! anySolo || trackSolo[t].load());
            const bool noFade = (t < kMaxTracks && trackNoFade[t]);              // Click/Guía
            const float cg = noFade ? 1.0f : ciG;                                // Click/Guía no suben en el conteo
            const float jf = noFade ? 1.0f : afG;                                // ni en el fundido del salto (downbeat crujiente y a tiempo)
            const float g = audible ? trackGain[t].load() * busGain[trackFamily[t]].load() * cg * jf : 0.0f;
            const float* tL = temp.getReadPointer (0);
            const float* tR = temp.getNumChannels() > 1 ? temp.getReadPointer (1) : tL;

            const int fi   = (t < kMaxTracks ? trackRouteFam[t] : 10);   // familia -> ruta de salida
            int mode = famMode[fi];
            int base = famBaseCh[fi];
            if (featSalidas <= 2 && base >= 2) base = 0;   // Basico: todo al par principal (1/2)
            if (autoPan.load())                            // Autopan (jack 2 salidas): Click(15)/Guia(13) -> derecha, resto -> izquierda
            {
                const bool clickOguia = (fi == 15 || fi == 13);
                mode = 1;                                  // mono
                base = (clickOguia && useCh >= 2) ? 1 : 0; // derecha (canal 1) o izquierda (canal 0)
            }
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
            if (featSalidas <= 2 && cb >= 2) cb = 0;   // Basico: click al par principal
            if (autoPan.load()) { cm = 1; cb = (useCh >= 2 ? 1 : 0); }   // Autopan: click sintetizado a la derecha
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

        // ── Guía dinámica: dispara y mezcla el cue de voz por la ruta/fader de "Guía" ──
        if (guideOn.load())
        {
            const long long blkStart = positionOut.load();
            // Conteo hablado: dispara en el beat EXACTO cortando lo anterior (ritmo de metrónomo).
            if (cntIdx < cntN && blkStart + nn > cntAt[cntIdx])
            {
                cueActiveBuf = cntClip[cntIdx]; cuePos = 0; ++cntIdx;
            }
            else if (cueActiveBuf < 0)   // disparo normal de sección: cuando llegamos al instante programado
            {
                const int pend = cuePendingIdx.load();
                const long long fireAt = cueFireAtSample.load();
                if (pend >= 0 && fireAt >= 0 && blkStart + nn > fireAt)
                { cueActiveBuf = pend; cuePos = 0; cuePendingIdx.store (-1); }
            }
            if (cueActiveBuf >= 0 && cueActiveBuf < (int) guideBuffers.size())
            {
                const auto& cbuf = guideBuffers[(size_t) cueActiveBuf];
                const int blen = cbuf.getNumSamples();
                if (blen <= 0) { cueActiveBuf = -1; }
                else
                {
                    const float* cs = cbuf.getReadPointer (0);
                    float gg = 1.0f; int fi = 10;
                    if (guiaTrkIdx >= 0 && guiaTrkIdx < kMaxTracks)
                    {
                        gg = trackGain[guiaTrkIdx].load() * busGain[trackFamily[guiaTrkIdx]].load();
                        if (trackMuted[guiaTrkIdx].load()) gg = 0.0f;
                        fi = trackRouteFam[guiaTrkIdx];
                    }
                    int cmode = famMode[fi], cbase = famBaseCh[fi];
                    if (featSalidas <= 2 && cbase >= 2) cbase = 0;
                    if (autoPan.load()) { cmode = 1; cbase = (useCh >= 2 ? 1 : 0); }
                    for (int n = 0; n < nn && cuePos < blen; ++n, ++cuePos)
                    {
                        const float s = cs[cuePos] * gg;
                        if (cmode == 2 && cbase >= 0 && cbase < useCh) { out[cbase][n] += s; if (cbase + 1 < useCh) out[cbase + 1][n] += s; }
                        else if (cbase >= 0 && cbase < useCh) out[cbase][n] += s;
                    }
                    if (cuePos >= blen) { cueActiveBuf = -1; cuePos = 0; }
                }
            }
        }
        else if (cueActiveBuf >= 0) { cueActiveBuf = -1; cuePos = 0; }

        const float m = masterGain.load();
        for (int c = 0; c < useCh; ++c) { float* o = out[c]; for (int n = 0; n < nn; ++n) o[n] = softClip (o[n] * m); }
        positionOut.fetch_add (nn);

        const double posSec = (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate);
        const double fr2 = fileRates.isEmpty() ? 44100.0 : fileRates[0];

        // ── Fase D: transición AUTOMÁTICA al terminar (armada). Pide al timer que haga la transición. ──
        if (autoTrans.load() != 0 && autoReqPending.load() == 0 && ! arrangementActive.load()
            && indiceSiguiente() >= 0)
        {
            const double outS = songOutSec.load();
            const double effEnd = (outS > 0.0) ? juce::jmin (outS, totalSeconds()) : totalSeconds();
            if (effEnd > 0.1)
            {
                if (autoTrans.load() == 2)   // Sobreponer: arrancar el cruce ~2 compases antes del final
                {
                    const double bars = (bpm > 0.0) ? 2.0 * juce::jmax (1, beatsPerBar) * 60.0 / bpm : 3.0;
                    if (posSec >= effEnd - bars - 0.02) autoReqPending.store (2);
                }
                else                         // Autolink: al llegar al final
                {
                    if (posSec >= effEnd - 0.02) autoReqPending.store (1);
                }
            }
        }

        // ── Guía dinámica: programa el cue de la PRÓXIMA sección, 1 compás antes del cambio ──
        const bool guiaOn = guideOn.load() && guideLoaded.load() && playing.load();
        const bool arrOn  = arrangementActive.load() && arrangement.size() > 0;
        const bool haySec = arrOn || sectionTimes.size() > 0;

        // (a) Al darle Play: armar el CONTEO hablado (uno,dos / uno,dos,tres,cuatro) sobre los 2 compases.
        //     Si la canción no tiene conteo, se anuncia la 1ª sección de una.
        if (guiaOn && ! guideWasPlaying)
        {
            cueSchedStep = -1000; linSchedSec = -1000; guideWasLoop = false;
            buildCountIn();
            // Arrancamos desde el inicio (en/antes del conteo)?
            const double songStart0 = sectionTimes.isEmpty() ? 0.0 : sectionTimes[0];
            const bool desdeInicio = posSec < songStart0 + 0.05;
            if (cntN == 0 && desdeInicio)   // sin conteo pero desde el inicio → anunciar la 1ª sección directo
            {
                const int firstSec = arrOn ? (arrangement[0] >= 0 ? arrangement[0] : -999)
                                           : (sectionTimes.size() > 0 ? 0 : -999);
                if (firstSec >= 0)
                {
                    const int clip = guideClipForSection (firstSec);
                    if (clip >= 0) { cueActiveBuf = clip; cuePos = 0; }
                }
            }
        }
        guideWasPlaying = guiaOn;
        // El conteo hablado aún no termina (y solo mientras estamos ANTES del inicio de la canción).
        const bool enConteoVoz = (cntIdx < cntN) && (sectionTimes.isEmpty() || posSec < sectionTimes[0] - 0.02);

        // (b) Anunciar la PRÓXIMA sección — o el DESTINO si hay un salto encolado — 1 compás antes.
        //     (No programa secciones mientras corre el conteo hablado del inicio.)
        if (guiaOn && haySec && ! enConteoVoz)
        {
            const bool enLoop = (loopActive.load() || loopOnce.load()) && loopEndSec.load() > 0.0;
            const int ctx = arrOn ? arrStep.load()     : liveSectionIndexAt (posSec);
            const int q   = arrOn ? queuedStep.load()  : queuedSection.load();
            if (ctx != cueSchedStep || q != linSchedSec || enLoop != guideWasLoop)
            {
                cueSchedStep = ctx; linSchedSec = q; guideWasLoop = enLoop;
                cuePendingIdx.store (-1);
                int annSec = -999; double fireSec = -1.0;
                if (enLoop)   // repetición: anunciar la MISMA sección, 1 compás antes de que vuelva a repetir
                {
                    annSec  = liveSectionIndexAt (loopStartSec.load());
                    fireSec = tiempoUnCompasAntes (loopEndSec.load());
                }
                else
                {
                    int curSecIdx = -999;
                    if (arrOn)
                    {
                        curSecIdx = (ctx >= 0 && ctx < arrangement.size()) ? arrangement[ctx] : -999;
                        if (q >= 0 && q < arrangement.size() && arrangement[q] >= 0) annSec = arrangement[q];        // destino encolado
                        else if (ctx + 1 < arrangement.size())                       annSec = arrangement[ctx + 1];  // siguiente
                        else if (arrLoop.load())                                     annSec = arrangement[0];
                    }
                    else
                    {
                        curSecIdx = ctx;
                        if (q >= 0 && q < sectionTimes.size())   annSec = q;          // destino encolado (índice de sección)
                        else if (ctx + 1 < sectionTimes.size())  annSec = ctx + 1;    // siguiente
                    }
                    if (curSecIdx != -999) fireSec = tiempoUnCompasAntes (secEndOf (curSecIdx));
                }
                if (annSec >= 0 && fireSec >= 0.0)
                {
                    const int clip = guideClipForSection (annSec);
                    if (clip >= 0)
                    {
                        cueFireAtSample.store ((long long) (juce::jmax (0.0, fireSec) * deviceSampleRate));
                        cuePendingIdx.store (clip);
                    }
                }
            }
        }
        else if (! guiaOn) { cueSchedStep = -1000; linSchedSec = -1000; guideWasLoop = false; }

        // ── Salto en vivo encolado (Fase B): al terminar la sección actual, saltar a la encolada ──
        bool boundaryHandled = false;
        // Arreglo: encolar un PASO; salta al terminar la sección del paso actual
        if (arrangementActive.load() && arrangement.size() > 0 && queuedStep.load() >= 0)
        {
            const int step = arrStep.load();
            const int secIdx = (step >= 0 && step < arrangement.size()) ? arrangement[step] : -999;
            if (secIdx != -999 && posSec >= secEndOf (secIdx) - 1.0e-4)
            {
                const int q = queuedStep.exchange (-1);
                if (q >= 0 && q < arrangement.size())
                {
                    arrStep.store (q);
                    const double tgt = secStartOf (arrangement[q]);
                    seekTo.store ((long long) (tgt * fr2));
                    arrFadeStart.store (tgt); arrFadeEnd.store (tgt + kArrFadeSec);   // anti-click
                    boundaryHandled = true;
                }
            }
        }
        if (! boundaryHandled && queuedSection.load() >= 0 && ! sectionTimes.isEmpty())
        {
            const int qf = queuedFromSec.load();   // fijo: sección donde se encoló (kConteoIdx = conteo)
            const double endQF = (qf == kConteoIdx) ? (sectionTimes.isEmpty() ? 0.0 : sectionTimes[0])
                               : ((qf >= 0 && qf + 1 < sectionTimes.size()) ? sectionTimes[qf + 1] : totalSeconds());
            if (posSec >= endQF - 1.0e-4)
            {
                const int q = queuedSection.exchange (-1);
                queuedFromSec.store (-1);
                queuedStep.store (-1);   // limpiar también el resaltado
                if (q >= 0 && q < sectionTimes.size())
                {
                    seekTo.store ((long long) (sectionTimes[q] * fr2));
                    arrFadeStart.store (sectionTimes[q]); arrFadeEnd.store (sectionTimes[q] + kArrFadeSec);   // anti-click
                    if (arrangementActive.load())   // sincronizar el paso del arreglo con el salto
                    {
                        int st = -1;
                        for (int k = 0; k < arrangement.size(); ++k) if (arrangement[k] == q) { st = k; break; }
                        if (st >= 0) arrStep.store (st);
                    }
                    boundaryHandled = true;
                }
            }
        }
        // ── Repetición (repetir/∞): PRIORIDAD sobre el avance del arreglo — repite la sección actual ──
        if (! boundaryHandled && (loopActive.load() || loopOnce.load())
            && loopEndSec.load() > 0.0 && posSec >= loopEndSec.load() - 1.0e-4)
        {
            const double ls = juce::jmax (0.0, loopStartSec.load());
            seekTo.store ((long long) (ls * fr2));
            arrFadeStart.store (ls); arrFadeEnd.store (ls + kArrFadeSec);   // anti-click
            if (loopOnce.load())
            {
                loopOnce.store (false);
                if (! loopActive.load()) { loopStartSec.store (-1.0); loopEndSec.store (-1.0); }
            }
            boundaryHandled = true;
        }
        // ── Arreglo activo (Fase A): al terminar cada sección, saltar al siguiente paso ──
        if (boundaryHandled)
        {
            // ya saltamos a la sección encolada; no aplicar el manejo normal de fin en este bloque
        }
        else if (arrangementActive.load() && arrangement.size() > 0)
        {
            const int step   = arrStep.load();
            const int secIdx = (step >= 0 && step < arrangement.size()) ? arrangement[step] : -999;
            if (secIdx != -999)
            {
                const double endSec = secEndOf (secIdx);
                if (posSec >= endSec - 1.0e-4)
                {
                    const int next = step + 1;
                    if (next < arrangement.size())
                    {
                        arrStep.store (next);
                        const double nStart = secStartOf (arrangement[next]);
                        // Solo saltamos si NO es contiguo (si lo es, seguimos sin cortar = seamless).
                        if (std::abs (nStart - endSec) > 1.0e-3)
                        {
                            seekTo.store ((long long) (nStart * fr2));
                            arrFadeStart.store (nStart); arrFadeEnd.store (nStart + kArrFadeSec);   // anti-click
                        }
                    }
                    else
                    {   // fin del arreglo: volver al inicio del arreglo (loop) o parar ahí
                        const double fStart = secStartOf (arrangement[0]);
                        arrStep.store (0);
                        if (! arrLoop.load()) playing.store (false);
                        seekTo.store ((long long) (fStart * fr2));
                        arrFadeStart.store (fStart); arrFadeEnd.store (fStart + kArrFadeSec);   // anti-click
                    }
                }
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
            // Si hay transición automática armada y pedida, NO parar: el timer hará la transición.
            const bool transPendiente = (autoTrans.load() != 0 && autoReqPending.load() != 0);
            if (endDev > 0 && positionOut.load() >= endDev && ! transPendiente)
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
        // Logo anclado a la fila del header (respeta el safe area en iPad, no una Y fija)
        drawLogo (g, (float) hdrLogoX, (float) hdrLogoY);

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
            const bool phBpm = npEsIPhone();
            g.setColour (juce::Colour (0xffe8e8e8));
            g.setFont (juce::Font (phBpm ? 10.0f : 12.5f, juce::Font::bold));
            g.drawText (juce::String (juce::roundToInt (currentBpm())) + " BPM",
                        compasBoxBounds.withHeight (hh).translated (0, 1), juce::Justification::centred, false);
            g.setColour (juce::Colour (0xffb0b0b0));
            g.setFont (juce::Font (phBpm ? 9.5f : 11.5f));
            g.drawText (songCompas, compasBoxBounds.withTrimmedTop (hh).translated (0, -1),
                        juce::Justification::centred, false);
        }

        // Indicador de NeuralSync (bajo el botón Play): solo cuando NeuralSync está activo.
        if (syncEnabled && ! syncBadgeBounds.isEmpty())
        {
            const bool lk = syncLinked.load();
            const juce::String txt = (lk ? juce::String::fromUTF8 ("NeuralSync conectado")
                                         : juce::String::fromUTF8 ("NeuralSync \xc2\xb7 esperando\xe2\x80\xa6"))
                                     + juce::String::fromUTF8 ("   ") + localLanIp() + ":" + juce::String (liveServer.port);   // IP (diagnóstico)
            juce::GlyphArrangement ga; ga.addLineOfText (juce::Font (12.5f, juce::Font::bold), txt, 0.0f, 0.0f);
            const float tw = ga.getBoundingBox (0, -1, true).getWidth();
            const float dotD = 9.0f, sp = 7.0f, total = dotD + sp + tw;
            const float sx = (float) syncBadgeBounds.getCentreX() - total * 0.5f;
            const float cy = (float) syncBadgeBounds.getCentreY();
            juce::Rectangle<float> dot (sx, cy - dotD * 0.5f, dotD, dotD);
            g.setColour (lk ? juce::Colour (0xff3ED66E) : juce::Colour (0xff6b6b6b));
            g.fillEllipse (dot);
            g.setColour (lk ? juce::Colour (0xffdfe8df) : juce::Colour (0xff9a9a9a));
            g.setFont (juce::Font (12.5f, juce::Font::bold));
            g.drawText (txt, juce::Rectangle<float> (sx + dotD + sp, cy - 9.0f, tw + 8.0f, 18.0f),
                        juce::Justification::centredLeft);
        }

        // Nombre del repertorio centrado (debajo del botón de Play)
        if (! setlistBandBounds.isEmpty() && currentSetlistName.isNotEmpty())
        {
            g.setColour (juce::Colour (0xffcfcfcf));
            g.setFont (juce::Font (13.5f, juce::Font::bold));
            g.drawText (currentSetlistName, setlistBandBounds, juce::Justification::centred, true);
        }

        // iPhone en vista Faders/Buses/Pad/MIDI el mapa está oculto (mapBounds vacío):
        // no dibujarlo, si no se generan rectángulos de tamaño negativo → crash.
        if (! mapBounds.isEmpty())
        {
        auto mb = mapBounds;
        g.setColour (juce::Colour (0xff0d0d0d));
        g.fillRoundedRectangle (mb.toFloat(), 8.0f);

        auto inner = mb.reduced (8);
        double vs = 0.0, ve = 0.0; getViewWindow (vs, ve);
        const double span = juce::jmax (0.001, ve - vs);

        // (El grid de beats se dibuja más abajo, en posición compactada, encima de la onda por sección.)

        const bool preparandoActual = (preparandoSongId >= 0 && currentSong < 0);   // tono preparándose, sin canción cargada
        if (preparandoActual)
        {
            g.setColour (juce::Colour (0xff9aa0a6)); g.setFont (14.5f);
            float pr = -1.0f;
            auto it = renderById.find (preparandoSongId);
            if (it != renderById.end()) pr = it->second;
            juce::String m = juce::String::fromUTF8 ("Preparando tu tono\xe2\x80\xa6");
            if (pr >= 0.0f) m += juce::String::fromUTF8 ("   ") + juce::String ((int) (pr * 100.0f)) + " %";
            g.drawText (m, inner, juce::Justification::centred, true);
        }
        else if (thumb.getTotalLength() > 0.0)
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
            else if (! repertoire.isEmpty())
            {
                // Mostrar el % real de la primera cancion (la que se cargara al terminar)
                float p = -1.0f;
                if (! dlById.empty())
                {
                    auto it = dlById.find (repertoire.getReference (0).id);
                    p = (it != dlById.end()) ? it->second : dlById.begin()->second;
                }
                if (p >= 0.0f)
                    msg = juce::String::fromUTF8 ("Descargando stems\xe2\x80\xa6 ")
                          + juce::String ((int) (p * 100.0f)) + juce::String::fromUTF8 (" %  \xc2\xb7  se guarda en cache");
                else
                    msg = juce::String::fromUTF8 ("Descargando stems\xe2\x80\xa6");
            }
            else if (serverToken.isEmpty())           msg = juce::String ("Conecta para traer el repertorio");
            else if (currentSetlistName.isNotEmpty()) msg = juce::String::fromUTF8 ("Empez\xc3\xa1 a agregar canciones a este repertorio con +");
            else                                      msg = juce::String::fromUTF8 ("Abr\xc3\xad un repertorio o cre\xc3\xa1 uno nuevo");
            g.drawText (msg, inner, juce::Justification::centred, true);
        }

        if (currentSong >= 0 && ! preparandoActual)   // sin canción o preparando: no dibujar secciones, "Click ∞", ni bloques
        {
            const double total = totalSeconds();
            const double posNow = positionSeconds();
            // t0/t1 = posición en pantalla (ya compactada). srcT0/srcT1 = tramo real de audio (para la onda).
            auto drawBlk = [&] (double t0, double t1, const juce::String& name, int idx = -1,
                                double srcT0 = -1.0, double srcT1 = -1.0)
            {
                if (t1 <= vs || t0 >= ve || t1 <= t0) return;
                const double c0 = juce::jmax (t0, vs);
                const double c1 = juce::jmin (t1, ve);
                const float x0 = inner.getX() + (float) ((c0 - vs) / span) * inner.getWidth();
                const float x1 = inner.getX() + (float) ((c1 - vs) / span) * inner.getWidth();
                juce::Rectangle<float> blk (x0 + 0.5f, (float) inner.getY() + 2.0f,
                                            juce::jmax (10.0f, x1 - x0 - 1.0f), (float) inner.getHeight() - 4.0f);
                // Onda del tramo real. Si el bloque está recortado por vs/ve, dibujar SOLO la porción
                // visible de la onda (si no, se estira al hacer scroll en reproducción).
                if (srcT0 >= 0.0 && srcT1 > srcT0 && thumb.getTotalLength() > 0.0)
                {
                    const double vSrc0 = srcT0 + (c0 - t0);   // display<->source es 1:1 (misma duración)
                    const double vSrc1 = srcT0 + (c1 - t0);
                    g.setColour (juce::Colour (0xffced1d6));
                    thumb.drawChannels (g, blk.toNearestInt().reduced (1), vSrc0, vSrc1, 1.2f);
                }
                const double posNowC = seqDisplayNow();   // cursor en tiempo display (t0/t1 ya lo están)
                const bool act = (posNowC >= t0 && posNowC < t1);
                const float rad = 4.0f;   // esquinas sutiles: las secciones se ven pegadas (continuas)
                // Relleno con degradado sutil (más claro arriba, más oscuro abajo)
                {
                    juce::ColourGradient grad (juce::Colour ((juce::uint32) (act ? 0x30ffffff : 0x0dffffff)), blk.getX(), blk.getY(),
                                               juce::Colour ((juce::uint32) (act ? 0x12ffffff : 0x05ffffff)), blk.getX(), blk.getBottom(), false);
                    g.setGradientFill (grad);
                    g.fillRoundedRectangle (blk, rad);
                }
                // Borde suave
                g.setColour (act ? juce::Colour (0x70ffffff) : juce::Colour (0x24ffffff));
                g.drawRoundedRectangle (blk, rad, 1.0f);
                // Etiqueta discreta: chica, en la esquina sup-izq, recuadro tenue
                {
                    g.setFont (juce::Font (10.5f, juce::Font::bold));
                    const float tw = juce::jmin (blk.getWidth() - 10.0f, (float) name.length() * 6.0f + 12.0f);
                    juce::Rectangle<float> chip (blk.getX() + 5.0f, blk.getY() + 4.0f, juce::jmax (14.0f, tw), 15.0f);
                    g.setColour (juce::Colour (0x59000000));
                    g.fillRoundedRectangle (chip, 4.0f);
                    g.setColour (act ? juce::Colours::white : juce::Colour (0xffc8ccd2));
                    g.drawText (name, chip.reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
                }

                // Fase B: guardar el rect del nombre (tap en vivo = encolar) y resaltar la encolada
                if (idx >= 0 && idx < arrChipRects.size())
                {
                    arrChipRects.set (idx, blk.toNearestInt());   // tap en cualquier parte del bloque encola
                    if (! editMode && idx == queuedStep.load())
                    {
                        g.setColour (juce::Colour (0xff3ED66E));
                        g.drawRoundedRectangle (blk.reduced (1.0f), 8.0f, 2.2f);
                        juce::Rectangle<float> tag (blk.getRight() - 24.0f, blk.getY() + 3.0f, 20.0f, 18.0f);
                        g.setColour (juce::Colour (0xff3ED66E)); g.setFont (juce::Font (15.0f, juce::Font::bold));
                        g.drawText (juce::String::fromUTF8 ("\xe2\x86\x92"), tag, juce::Justification::centred);  // →
                    }
                }

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

                // ── Editor (modo Editar): − quita este slot / + inserta una sección después ──
                if (editMode && idx >= 0)
                {
                    const int r = 10;
                    juce::Rectangle<int> minus ((int) blk.getX() + 5, (int) blk.getBottom() - 2 * r - 5, 2 * r, 2 * r);
                    g.setColour (juce::Colour (0xffE5534B)); g.fillEllipse (minus.toFloat());
                    g.setColour (juce::Colours::white); g.setFont (juce::Font (14.0f, juce::Font::bold));
                    g.drawText (juce::String::fromUTF8 ("\xe2\x88\x92"), minus, juce::Justification::centred);
                    slotMinusRect[idx] = minus;
                    if (blk.getWidth() > 52.0f)   // + (azul) a la derecha: insertar sección después de este slot
                    {
                        juce::Rectangle<int> plus ((int) blk.getRight() - 2 * r - 5, (int) blk.getBottom() - 2 * r - 5, 2 * r, 2 * r);
                        g.setColour (juce::Colour (0xff2E6BE6)); g.fillEllipse (plus.toFloat());
                        g.setColour (juce::Colours::white); g.setFont (juce::Font (14.0f, juce::Font::bold));
                        g.drawText ("+", plus, juce::Justification::centred);
                        slotPlusRect[idx] = plus;
                    }
                }

            };

            // Cubrimos la onda continua; se redibuja por SLOT de la secuencia en su posición display
            g.setColour (juce::Colour (0xff0d0d0d));
            g.fillRect (inner);

            const juce::Array<int> seq = mapSeq();
            arrChipRects.clearQuick();
            for (int k = 0; k < seq.size(); ++k) arrChipRects.add ({});
            if (editMode) { slotMinusRect.clear(); slotPlusRect.clear(); }
            addChipRect = {};

            // Dibuja cada SLOT de la secuencia, pegados (compactados). Un slot repetido se dibuja varias veces.
            double disp = 0.0;
            for (int k = 0; k < seq.size(); ++k)
            {
                const int s = seq[k];
                const double d = seqDur (s);
                drawBlk (disp, disp + d, secNameOf (s), k, secStartOf (s), secEndOf (s));
                // Rayitas de beats de este slot, en su posición display
                if (bpm > 0.0)
                {
                    const double secPerEighth = 30.0 / bpm;
                    const int perBar = juce::jmax (1, beatsPerBar) * 2;
                    const double rs0 = secStartOf (s), rs1 = secEndOf (s), sh = rs0 - disp;
                    for (int j = (int) std::ceil (rs0 / secPerEighth - 1.0e-6); j * secPerEighth < rs1 - 1.0e-6; ++j)
                    {
                        const double t = j * secPerEighth;
                        if (t < rs0 - 1.0e-6 || t < 0.0) continue;
                        const double dt = t - sh;
                        if (dt < vs || dt > ve) continue;
                        const float gx = inner.getX() + (float) ((dt - vs) / span) * inner.getWidth();
                        float a = 0.035f; if (j % 2 == 0) a = 0.085f; if (j % perBar == 0) a = 0.17f;
                        g.setColour (juce::Colours::white.withAlpha (a));
                        g.drawVerticalLine ((int) gx, (float) inner.getY(), (float) inner.getBottom());
                    }
                }
                disp += d;
            }

            // #3 sección de click: bloque anexo "Click ∞" + agregar/quitar (modo edición)
            addClickBtnRect = {}; delClickBtnRect = {};
            const double lastEnd = disp;   // pegado al final del mapa (largo display de la secuencia)
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

        // Playhead en vivo, en posición DISPLAY de la secuencia
        if (currentSong >= 0 && ! editMode)
        {
            const double dpos = seqDisplayNow();
            if (dpos >= vs && dpos <= ve)
            {
                const float px = inner.getX() + (float) ((dpos - vs) / span) * inner.getWidth();
                g.setColour (juce::Colours::white.withAlpha (0.10f));
                g.fillRect (juce::Rectangle<float> ((float) inner.getX(), (float) inner.getY(), px - inner.getX(), (float) inner.getHeight()));
                g.setColour (juce::Colours::white);
                g.fillRect (px - 1.0f, (float) inner.getY(), 2.0f, (float) inner.getHeight());
            }
        }

        // Marcadores de notas MIDI: solo en vivo y sin arreglo (en arreglo las posiciones no coinciden 1:1)
        if (! editMode && ! arrangementActive.load())
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

        // (El mapping fuera de Editar NUNCA se compacta: siempre la canción completa, lineal.)
        }   // ── fin del bloque del mapa (solo si mapBounds no está vacío) ──

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
        if (editMode)   // editor de secuencia: − quita el slot / + inserta una sección después
        {
            for (auto& kv : slotMinusRect)
                if (! kv.second.isEmpty() && kv.second.contains (e.getPosition()))
                { if (kv.first >= 0 && kv.first < editSeq.size()) editSeq.remove (kv.first); repaint (mapBounds); return; }
            for (auto& kv : slotPlusRect)
                if (! kv.second.isEmpty() && kv.second.contains (e.getPosition()))
                { mostrarMenuInsertar (kv.first); return; }
        }
        pendingChipK = -1;
        if (! editMode)   // en vivo: un TAP en un slot encola el salto; un ARRASTRE desliza el mapa
        {
            for (int k = 0; k < arrChipRects.size(); ++k)
                if (! arrChipRects[k].isEmpty() && arrChipRects[k].contains (e.getPosition()))
                { pendingChipK = k; break; }   // recordar; la acción se decide en mouseUp (tap vs drag)
        }
        if (! mapBounds.contains (e.getPosition())) return;
        isDragging = true;
        dragSeeks = ! playing.load();
        dragStartCenter = browsing ? browseCenter : seqDisplayNow();   // centro en tiempo display
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! isDragging) return;
        auto inner = mapBounds.reduced (8);
        const double win = juce::jmin (20.0, seqTotalOf (mapSeq()));
        const double dx = e.getDistanceFromDragStartX() * (win / juce::jmax (1, inner.getWidth())) * npMapDragSens();
        lastInteractionMs = juce::Time::getMillisecondCounter();
        const double dt = dragStartCenter - dx;
        if (dragSeeks)
        {
            seekToDisplay (dt);   // el drag va en tiempo display -> real
            // En arreglo, seqDisplayNow() va con atraso (el seek es asíncrono y arrStep cambia ya):
            // seguimos la vista con el valor arrastrado para que NO se jale/salte.
            if (arrangementActive.load())
            { browsing = true; browseCenter = juce::jlimit (0.0, seqTotalOf (mapSeq()), dt); }
            repaint (mapBounds);
        }
        else { browsing = true; browseCenter = juce::jlimit (0.0, seqTotalOf (mapSeq()), dt); repaint (mapBounds); }
    }
    // Acción de TAP sobre un slot del mapa (en vivo): encola el salto o salta si está parado.
    void tapChip (int k)
    {
        if (! guideOn.load()) return;   // saltar a secciones solo en modo Guía Neural
        const juce::Array<int> seq = mapSeq();
        if (k < 0 || k >= seq.size()) return;
        const int sec = seq[k];
        if (playing.load())   // reproduciendo: encolar (salta al terminar la sección actual)
        {
            if (queuedStep.load() == k) { queuedStep.store (-1); queuedSection.store (-1); queuedFromSec.store (-1); }  // tocar de nuevo = cancelar
            else
            {
                queuedStep.store (k);   // resaltado + salto (arreglo)
                if (! arrangementActive.load() && sec != kConteoIdx)   // modo natural: encolar por sección
                {
                    const double p = positionSeconds();
                    const bool enConteo = (! sectionTimes.isEmpty() && p < sectionTimes[0] - 1.0e-3);
                    queuedSection.store (sec);
                    queuedFromSec.store (enConteo ? kConteoIdx : liveSectionIndexAt (p));
                }
            }
        }
        else   // parado: salto inmediato (para posicionarse)
        {
            seekSeconds (secStartOf (sec));
            if (arrangementActive.load()) arrStep.store (k);
        }
        repaint (mapBounds);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isDragging) return;
        isDragging = false;
        const bool wasTap = e.getDistanceFromDragStart() < 5;
        if (! editMode && pendingChipK >= 0 && wasTap)   // tap en un slot → encolar/saltar
        { const int k = pendingChipK; pendingChipK = -1; tapChip (k); return; }
        pendingChipK = -1;
        if (dragSeeks)
        {
            if (e.getDistanceFromDragStart() < 5) seekFromMouse (e);          // clic simple
            else
            {                                                                // al soltar el arrastre, cae en un click
                auto inner = mapBounds.reduced (8);
                const double win = juce::jmin (20.0, seqTotalOf (mapSeq()));
                const double dx = e.getDistanceFromDragStartX() * (win / juce::jmax (1, inner.getWidth())) * npMapDragSens();
                seekToDisplay (dragStartCenter - dx);
            }
            browsing = false;   // la vista vuelve a seguir el cursor real (ya aplicado el seek)
            repaint (mapBounds);
        }
        else   // en vivo: mantener la posición navegada al soltar; el reloj de auto-regreso arranca ahora
        {
            if (! playing.load()) browsing = false;
            else lastInteractionMs = juce::Time::getMillisecondCounter();
            repaint (mapBounds);
        }
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
        // Scroll de dos dedos: en tiempo DISPLAY (compactado del arreglo), igual que el arrastre.
        // Antes iba en tiempo real y en los arreglos se atoraba (real != display).
        if (playing.load())
        {
            if (! browsing) browseCenter = seqDisplayNow();
            browsing = true;
            browseCenter = juce::jlimit (0.0, seqTotalOf (mapSeq()), browseCenter + d * 14.0);
            lastInteractionMs = juce::Time::getMillisecondCounter();
            repaint (mapBounds);
        }
        else seekToDisplay (seqDisplayNow() + d * 14.0);
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
       #if JUCE_IOS || JUCE_ANDROID
        if (loginOverlay != nullptr && loginOverlay->isVisible())
            loginOverlay->setBounds (getLocalBounds());
       #endif

        auto full = getLocalBounds();
       #if JUCE_IOS || JUCE_ANDROID
        // Respetar las areas seguras del iPad (barra de estado / indicador / notch).
        if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            full = d->safeAreaInsets.subtractedFrom (full);
       #endif
        const bool phone = npEsIPhone();   // iPhone: interfaz compacta (pantalla chica)
        auto area = full.reduced (phone ? 8 : 16);

        // Barra superior: logo (izq) | Play + Inicio (centro) | Conectar + tiempo (der)
        auto topbar = area.removeFromTop (phone ? 40 : 46);
        {
            const int BW = phone ? 60 : 80, BH = phone ? 30 : 34, G = phone ? 6 : 8;
            const int gy = topbar.getCentreY() - BH / 2;

            // Izquierda (tras el logo): caja de tiempo + caja de tempo/compás + PAD
            // logoW deja espacio para el wordmark "NeuralPlay" (mas ancho que el icono cuadrado)
            const int logoW = phone ? 92 : 122, boxW = phone ? 46 : 56, boxG = 6;
            hdrLogoX = topbar.getX() + 12;   // logo a la izquierda del header
            hdrLogoY = gy;                     // misma fila que reloj/PAD/Play (respeta safe area)
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
            transBtn.setBounds     (cx + BW / 2 + G + BW + G, gy, BW, BH);
            transBtn.setVisible    (! editMode && indiceSiguiente() >= 0);
        }
        connStatus.setVisible (false);
        area.removeFromTop (phone ? 1 : 6);   // iPhone: subir un poco las tarjetas

        // Franja del indicador de NeuralSync (bajo el Play), solo si NeuralSync está activo
        if (syncEnabled) { syncBadgeBounds = area.removeFromTop (20); area.removeFromTop (3); }
        else             syncBadgeBounds = {};

        // Franja con el nombre del repertorio centrado (solo si hay uno cargado)
        if (currentSetlistName.isNotEmpty()) { setlistBandBounds = area.removeFromTop (22); area.removeFromTop (phone ? 1 : 3); }
        else                                   setlistBandBounds = {};

        // Tarjetas verticales grandes: portada arriba + nombre abajo (scroll horizontal)
        auto strip = area.removeFromTop (phone ? 104 : 180);
        stripBounds = strip;
        {
            const int cardW = phone ? 148 : 240, cardH = phone ? 100 : 178;
            const int step = phone ? 160 : 250;
            const int n = songCards.size() + (editMode ? 1 : 0);
            const int contentW = juce::jmax (0, n * step - 10);
            stripScroll = juce::jlimit (0, juce::jmax (0, contentW - strip.getWidth()), stripScroll);
            int x = strip.getX() - stripScroll;
            for (auto* c : songCards) { c->setBounds (x, strip.getY(), cardW, cardH); x += step; }
            if (editMode) addCard.setBounds (x, strip.getY(), cardW, cardH);
        }
        area.removeFromTop (4);

        // ── iPhone: escenario único. La región inferior muestra UNA sola cosa a la vez
        //    (Mapa o Faders/Buses/Pad/MIDI). La columna de botones y el Master quedan
        //    siempre a la derecha para poder alternar entre vistas. ──
        if (phone)
        {
            editBarBounds = {};
            auto lower = area;
            const int sepPad = 12, btnColW = 46, midGap = 6, mColW = 54;
            const int rightW = sepPad + btnColW + midGap + mColW + 6;
            auto fixed = lower.removeFromRight (rightW);
            auto stage = lower;
            // En edición: fila compacta de botones (Mapeo / Guardar arreglo) arriba del escenario,
            // reduciendo el mapa para que quepa (igual que Mac/iPad, pero a medida del iPhone).
            if (editMode)
            {
                auto ebar = stage.removeFromTop (30); stage.removeFromTop (4);
                editBarBounds = ebar;
                auto b = ebar;
                const int g2 = 6;
                keyMapBtn.setBounds   (b.removeFromLeft (118)); b.removeFromLeft (g2);
                midiMapBtn.setBounds  (b.removeFromLeft (104)); b.removeFromLeft (g2);
                saveArrBtn.setBounds  (b.removeFromLeft (118)); b.removeFromLeft (g2);
                saveAsNewBtn.setBounds (b.removeFromLeft (150));
                keyMapBtn.toFront (false); midiMapBtn.toFront (false);
                saveArrBtn.toFront (false); saveAsNewBtn.toFront (false);
            }
            faderPanelBounds = stage;
            faderViewport.setBounds (stage);
            midiPanel.setBounds (stage);
            padPanel.setBounds (stage);
            if (phoneShowMap)
            {
                mapBounds = stage.reduced (0, 2);
                const int bw = 26, bh = 40, cy = mapBounds.getCentreY() - bh / 2;
                barPrevBtn.setBounds (mapBounds.getX() + 4, cy, bw, bh);
                barNextBtn.setBounds (mapBounds.getRight() - bw - 4, cy, bw, bh);
                const bool showNav = (currentSong >= 0);
                barPrevBtn.setVisible (showNav);
                barNextBtn.setVisible (showNav);
            }
            else
            {
                mapBounds = {};
                barPrevBtn.setVisible (false);
                barNextBtn.setVisible (false);
                if (faderView == 0 || faderView == 1) layoutFaderStrip();
            }
            // Columna derecha (siempre visible): 6 botones + Master
            auto fx = fixed.reduced (0, 6);
            masterSepX = fixed.getX() + 6;
            fx.removeFromLeft (sepPad);
            auto btnCol = fx.removeFromLeft (btnColW);
            fx.removeFromLeft (midGap);
            auto mcol = fx.removeFromRight (mColW);
            {
                juce::Button* btns[6] = { &faderViewBtn, &busesBtn, &padPlayerBtn, &muteMidiBtn, &repeatBtn, &infiniteBtn };
                const int bgap = 4;
                const int bh2 = (btnCol.getHeight() - bgap * 5) / 6;
                for (int i = 0; i < 6; ++i)
                {
                    btns[i]->setBounds (btnCol.removeFromTop (bh2));
                    if (i < 5) btnCol.removeFromTop (bgap);
                }
            }
            masterLabel.setBounds (mcol.removeFromBottom (16));
            masterSlider.setBounds (mcol.reduced (4, 0));

            splash.setBounds (getLocalBounds());
            repPicker.setBounds (getLocalBounds());
            settingsPanel.setBounds (getLocalBounds());
            storagePanel.setBounds (getLocalBounds());
            audioCfg.setBounds (getLocalBounds());
            repEdit.setBounds (getLocalBounds());
            return;
        }

        mapBounds = area.removeFromTop (phone ? 80 : 188);
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
            editBarBounds = juce::Rectangle<int> (bar.getX(), bar.getY(), 760, bar.getHeight());
            auto b = editBarBounds;
            keyMapBtn.setBounds (b.removeFromLeft (172)); b.removeFromLeft (6);
            midiMapBtn.setBounds (b.removeFromLeft (128)); b.removeFromLeft (6);
            saveArrBtn.setBounds (b.removeFromLeft (140)); b.removeFromLeft (6);
            saveAsNewBtn.setBounds (b.removeFromLeft (180));
            keyMapBtn.toFront (false); midiMapBtn.toFront (false); saveArrBtn.toFront (false); saveAsNewBtn.toFront (false);
            area.removeFromTop (5);
        }
        else area.removeFromTop (8);
        faderPanelBounds = area;

        // Region FIJA a la derecha: separador doble + 6 botones + Master (no se desplazan)
        const int sepPad   = phone ? 32 : 20;   // separación entre las líneas y el riel de botones
        const int btnColW  = phone ? 50 : 62;
        const int midGap   = phone ? 8  : 10;
        const int mColW    = phone ? 62 : 78;
        const int rightW = sepPad + btnColW + midGap + mColW + 8;
        auto fixed = area.removeFromRight (rightW);

        // Los tracks (solo esos) van en el viewport desplazable
        faderViewport.setBounds (area);
        midiPanel.setBounds (area);
        padPanel.setBounds (area);
        layoutFaderStrip();

        auto fx = fixed.reduced (0, phone ? 8 : 14);
        masterSepX = fixed.getX() + (phone ? 7 : 10);   // doble linea (coords MainComponent)
        fx.removeFromLeft (sepPad);
        auto btnCol = fx.removeFromLeft (btnColW);
        fx.removeFromLeft (midGap);
        auto mcol = fx.removeFromRight (mColW);
        {
            juce::Button* btns[6] = { &faderViewBtn, &busesBtn, &padPlayerBtn, &muteMidiBtn, &repeatBtn, &infiniteBtn };
            const int bgap = phone ? 4 : 6;
            const int bh = (btnCol.getHeight() - bgap * 5) / 6;
            for (int i = 0; i < 6; ++i)
            {
                btns[i]->setBounds (btnCol.removeFromTop (bh));
                if (i < 5) btnCol.removeFromTop (bgap);
            }
        }
        masterLabel.setBounds (mcol.removeFromBottom (phone ? 18 : 22));
        masterSlider.setBounds (mcol.reduced (phone ? 4 : 6, 0));

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
        // Esquina del header: wordmark "NeuralPlay" (logo interno), no el icono cuadrado.
        const juce::Image& corner = logoInternoImg.isValid() ? logoInternoImg : logoImg;
        if (corner.isValid())
        {
            const float hh = npEsIPhone() ? 24.0f : 34.0f;   // en iPhone el wordmark va más chico
            const float ww = hh * (float) corner.getWidth() / (float) juce::jmax (1, corner.getHeight());
            const float ly = npEsIPhone() ? (y + (34.0f - hh) * 0.5f) : y;   // centrado vertical en la fila
            g.drawImage (corner, juce::Rectangle<float> (x, ly, ww, hh), juce::RectanglePlacement::centred);
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
        serverSession = v.getProperty ("session", "").toString();
        npSessionToken = serverSession;   // activar el guardia de sesión única
        // Sesión única: si hay token pero NO token de sesión (login de una versión
        // anterior), forzamos re-login para activarla en este dispositivo.
        if (serverToken.isNotEmpty() && serverSession.isEmpty())
        {
            serverToken.clear();
            npSessionToken.clear();
        }
        featMidi     = (bool) v.getProperty ("feat_midi", true);
        featSalidas  = npCapSalidas ((int) v.getProperty ("feat_salidas", 32));
        featInfinito = (bool) v.getProperty ("feat_infinito", true);
        audioCfg.maxChans = featSalidas;
        fetchPadPacks();
        loadCachedPerfiles();
        fetchPerfiles();
        fetchArreglosServer();   // arreglos por organización (con caché local)
        fetchPlan();
    }

    void fetchPlan()   // refresca las features del plan (MIDI, etc.) desde el servidor
    {
        if (serverUrl.isEmpty() || serverToken.isEmpty()) return;
        const juce::String url = serverUrl + "/api/live/plan";
        const juce::String tok = serverToken;
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, url, tok]
        {
            auto v = juce::JSON::parse (httpGet (url, tok));
            juce::MessageManager::callAsync ([sp, v]
            {
                if (sp == nullptr) return;
                auto feats = v.getProperty ("features", juce::var());
                if (! sp->planIncluyeNeuralPlay (feats))   // el plan cambio a "sync": cerrar sesion
                {
                    const auto msg = sp->msgPlanSinNeuralPlay (feats);
                    sp->serverToken.clear();
                    sp->guardarConfigCuenta();
                    sp->connStatus.setText (msg, juce::dontSendNotification);
                    sp->mostrarLoginDialog();
                    if (sp->loginOverlay != nullptr) sp->loginOverlay->showError (msg);
                    return;
                }
                sp->aplicarPlan (feats);
            });
        });
    }

    // ── Cuenta / login (multi-tenant): email+password → token de la organización ──
    void guardarConfigCuenta()
    {
        auto f = npAppDir().getChildFile ("config.json");
        juce::var v;
        if (f.existsAsFile()) v = juce::JSON::parse (f.loadFileAsString());
        auto* o = v.getDynamicObject();
        if (o == nullptr) { o = new juce::DynamicObject(); v = juce::var (o); }
        o->setProperty ("serverUrl", serverUrl);
        o->setProperty ("token", serverToken);
        o->setProperty ("session", serverSession);
        o->setProperty ("feat_midi", featMidi);
        o->setProperty ("feat_salidas", featSalidas);
        o->setProperty ("feat_infinito", featInfinito);
        f.getParentDirectory().createDirectory();
        f.replaceWithText (juce::JSON::toString (v));
    }

    void doLogin (juce::String url, juce::String email, juce::String password)
    {
        url = url.trim();
        while (url.endsWithChar ('/')) url = url.dropLastCharacters (1);
        if (url.isEmpty()) url = "https://neuralworship.com";
        connStatus.setText (juce::String::fromUTF8 ("Iniciando sesion\xe2\x80\xa6"), juce::dontSendNotification);
        const juce::String body = "{\"email\":" + juce::JSON::toString (juce::var (email))
                                + ",\"password\":" + juce::JSON::toString (juce::var (password))
                                + ",\"app\":\"neuralplay\"}";   // el servidor rechaza a no-admin
        const juce::String loginUrl = url + "/api/auth/login";
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, loginUrl, url, body, email, password]
        {
            auto resp = httpPostJson (loginUrl, body);
            juce::MessageManager::callAsync ([sp, url, resp, email, password]
            {
                if (sp == nullptr) return;
                auto v = juce::JSON::parse (resp);
                if ((bool) v.getProperty ("ok", false))
                {
                    auto feats = v.getProperty ("features", juce::var());
                    if (! sp->planIncluyeNeuralPlay (feats))   // plan "sync": no entra a NeuralPlay
                    {
                        const auto msg = sp->msgPlanSinNeuralPlay (feats);
                        sp->serverToken.clear();
                        sp->connStatus.setText (msg, juce::dontSendNotification);
                       #if JUCE_IOS || JUCE_ANDROID
                        if (sp->loginOverlay != nullptr && sp->loginOverlay->isVisible())
                            sp->loginOverlay->showError (msg);
                        else
                       #endif
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                            juce::String::fromUTF8 ("Plan sin NeuralPlay"), msg);
                        return;
                    }
                    // Defensa extra: NeuralPlay es solo para administradores.
                    if (v.getProperty ("rol", "").toString() != "admin")
                    {
                        const auto msg = juce::String::fromUTF8 (
                            "NeuralPlay es solo para administradores. Los m\xc3\xbasicos usan NeuralCharts.");
                        sp->serverToken.clear();
                        sp->connStatus.setText (msg, juce::dontSendNotification);
                        if (sp->loginOverlay != nullptr && sp->loginOverlay->isVisible())
                            sp->loginOverlay->showError (msg);
                        else
                            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                juce::String::fromUTF8 ("Solo administradores"), msg);
                        return;
                    }
                    sp->serverUrl   = url;
                    sp->serverToken = v.getProperty ("token", "").toString();
                    // Sesion unica por dispositivo: guardar el token de sesion y reactivar el guardia.
                    sp->serverSession = v.getProperty ("session", "").toString();
                    npSessionToken = sp->serverSession;
                    npKickAvisado.store (false);
                    // Nueva sesion: empezar con el repertorio limpio (no arrastrar el de la cuenta anterior)
                    sp->lastSetlistId.clear();
                    sp->currentSetlistName.clear();
                    sp->repertoire.clearQuick();
                    sp->rebuildRepertoireStrip();
                    sp->clearSong();
                    sp->refreshEditAvailability();
                    sp->repaint();
                    sp->aplicarPlan (feats);
                    sp->fetchGuideClips();   // guía dinámica: precargar la librería de clips de voz
                    sp->guardarConfigCuenta();
                    // "Recordar mi contraseña": guardar/borrar en el Keychain del sistema.
                    if (sp->loginOverlay != nullptr && sp->loginOverlay->quiereRecordar())
                    {
                        npkc::set ("correo", email);
                        npkc::set ("clave",  password);
                    }
                    else
                    {
                        npkc::remove ("correo");
                        npkc::remove ("clave");
                    }
                    sp->fetchPadPacks();
                    sp->fetchPerfiles();
                    sp->fetchArreglosServer();   // arreglos por organización (con caché local)
                    if (sp->repPicker.isVisible()) sp->openRepertoirePicker();
                    sp->connStatus.setText (juce::String::fromUTF8 ("Sesion: ")
                                            + v.getProperty ("org_nombre", "").toString(),
                                            juce::dontSendNotification);
                    if (sp->loginOverlay != nullptr) { sp->loginOverlay->setVisible (false); sp->loginOverlay->reset(); }
                }
                else
                {
                    auto msg = v.getProperty ("mensaje", "").toString();
                    if (msg.isEmpty()) msg = juce::String::fromUTF8 ("No se pudo conectar. Revisa el servidor y tu conexion.");
                    sp->connStatus.setText (msg, juce::dontSendNotification);
                    if (sp->loginOverlay != nullptr && sp->loginOverlay->isVisible())
                        sp->loginOverlay->showError (msg);
                    else
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                juce::String::fromUTF8 ("Iniciar sesion"), msg);
                }
            });
        });
    }

    // En tactil (iOS/Android) un AlertWindow como ventana modal aparte no se
    // presenta sobre la ventana principal a pantalla completa. Este helper lo
    // monta como hijo del componente principal cubriendo toda la pantalla.
    // En escritorio no hace nada (el AlertWindow va a su propia ventana).
    void presentModalAlert (juce::AlertWindow* aw)
    {
       #if JUCE_IOS || JUCE_ANDROID
        addChildComponent (aw);
        aw->setAlwaysOnTop (true);
        aw->setBounds (getLocalBounds());
        aw->setVisible (true);
        aw->toFront (true);
       #else
        juce::ignoreUnused (aw);
       #endif
    }

    void mostrarLoginDialog()
    {
        // Overlay de login a pantalla completa (tapa toda la interfaz) en TODAS las plataformas.
        if (loginOverlay == nullptr)
        {
            loginOverlay = std::make_unique<NeuralLoginOverlay>();
            loginOverlay->onSubmit = [this] (juce::String e, juce::String p)
            {
                const juce::String srv = serverUrl.isNotEmpty() ? serverUrl
                                                                : juce::String ("https://neuralworship.com");
                doLogin (srv, e, p);
            };
            addAndMakeVisible (*loginOverlay);
        }
        loginOverlay->reset();
        loginOverlay->setLogo (logoImg);
        // Prellenar con las credenciales recordadas (Keychain del sistema), si las hay.
        {
            const juce::String savedEmail = npkc::get ("correo");
            const juce::String savedPass  = npkc::get ("clave");
            loginOverlay->prefill (savedEmail, savedPass, savedPass.isNotEmpty());
        }
        loginOverlay->setBounds (getLocalBounds());
        loginOverlay->setVisible (true);
        loginOverlay->toFront (true);
    }

    void reportarProblema()   // "Reportar un problema": manda el texto al servidor (/api/reporte)
    {
        if (serverUrl.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8 ("Sin sesi\xc3\xb3n"),
                juce::String::fromUTF8 ("Inici\xc3\xa1 sesi\xc3\xb3n para poder enviar un reporte."));
            return;
        }
        auto* w = new juce::AlertWindow (
            juce::String::fromUTF8 ("Reportar un problema"),
            juce::String::fromUTF8 ("Contanos qu\xc3\xa9 pas\xc3\xb3: qu\xc3\xa9 hac\xc3\xad""as, qu\xc3\xa9 esperabas y qu\xc3\xa9 ocurri\xc3\xb3."),
            juce::MessageBoxIconType::NoIcon);
        static NPAlertLnF npAlertLnf;
        w->setLookAndFeel (&npAlertLnf);
        w->addTextEditor ("msg", juce::String(), juce::String(), false);
        if (auto* te = w->getTextEditor ("msg"))
        {
            te->setMultiLine (true, true);
            te->setReturnKeyStartsNewLine (false);
            te->setSize (360, 110);
        }
        w->addButton (juce::String::fromUTF8 ("Enviar"),   1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (juce::String::fromUTF8 ("Correo a soporte"), 2);
        w->addButton (juce::String::fromUTF8 ("Cancelar"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

        const juce::String url = serverUrl, sess = serverSession;
        juce::String ver = "";
        if (auto* app = juce::JUCEApplication::getInstance()) ver = app->getApplicationVersion();
        const juce::String device = juce::SystemStats::getOperatingSystemName()
                                    + " / " + juce::SystemStats::getDeviceDescription();
        w->enterModalState (true, juce::ModalCallbackFunction::create (
            [w, url, sess, ver, device] (int res)
            {
                std::unique_ptr<juce::AlertWindow> holder (w);
                w->setLookAndFeel (nullptr);
                if (res == 2) {
                    juce::URL ("mailto:soporte@neuralworship.com?subject=Soporte%20NeuralPlay")
                        .launchInDefaultBrowser();
                    return;
                }
                if (res != 1) return;
                const juce::String msg = w->getTextEditorContents ("msg").trim();
                if (msg.isEmpty()) return;
                juce::DynamicObject::Ptr o = new juce::DynamicObject();
                o->setProperty ("mensaje", msg);
                o->setProperty ("app", "neuralplay");
                o->setProperty ("version", ver);
                o->setProperty ("device", device);
                o->setProperty ("session", sess);
                const juce::String body = juce::JSON::toString (juce::var (o.get()));
                const juce::String endpoint = url + "/api/reporte";
                juce::Thread::launch ([endpoint, body]
                {
                    const juce::String resp = httpPostJson (endpoint, body);
                    const bool ok = resp.contains ("\"ok\"") && resp.contains ("true");
                    juce::MessageManager::callAsync ([ok]
                    {
                        juce::AlertWindow::showMessageBoxAsync (
                            ok ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                            juce::String::fromUTF8 (ok ? "\xc2\xa1Gracias!" : "No se pudo enviar"),
                            juce::String::fromUTF8 (ok
                                ? "Recibimos tu reporte. Gracias por avisarnos."
                                : "No se pudo enviar el reporte. Revis\xc3\xa1 tu conexi\xc3\xb3n e intent\xc3\xa1 de nuevo."));
                    });
                });
            }), false);
    }

    void cerrarSesion()   // "Cambiar cuenta": olvida el token y pide login de nuevo
    {
        // Avisar al servidor para cerrar la sesión única (best-effort, en background).
        if (serverSession.isNotEmpty() && serverUrl.isNotEmpty())
        {
            const juce::String url = serverUrl + "/api/auth/logout", sess = serverSession;
            juce::Thread::launch ([url, sess]
            {
                juce::URL u = juce::URL (url).withPOSTData (juce::String ("{}"));
                auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                .withExtraHeaders ("Content-Type: application/json\r\nX-Session-Token: " + sess)
                                .withConnectionTimeoutMs (6000);
                std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
                if (in != nullptr) in->readEntireStreamAsString();
            });
        }
        serverToken.clear();
        serverSession.clear();
        npSessionToken.clear();
        npKickAvisado.store (false);
        guardarConfigCuenta();
        // Limpiar el repertorio y la cancion cargada en memoria (no mostrar los de la cuenta anterior)
        lastSetlistId.clear();
        currentSetlistName.clear();
        repertoire.clearQuick();
        rebuildRepertoireStrip();
        clearSong();
        refreshEditAvailability();
        // Se CONSERVAN los stems ya descargados (npCacheDir) para no volver a bajarlos al reingresar.
        // La caché de audio se administra manualmente desde el panel «Almacenamiento».
        // Solo se limpia el roster de la organización anterior.
        npAppDir().getChildFile ("perfiles.json").deleteFile();
        { const juce::ScopedLock l (chartLock); perfilesJson = "[]"; }
        repaint();
        mostrarLoginDialog();
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

    // ── Arreglos por organización (servidor) ──────────────────────
    void fetchArreglosServer()   // baja los arreglos de la org y actualiza el caché local
    {
        if (serverUrl.isEmpty() || serverToken.isEmpty()) return;
        const juce::String url = serverUrl + "/api/live/arreglos";
        const juce::String tok = serverToken;
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, url, tok]
        {
            auto v = juce::JSON::parse (httpGet (url, tok));
            if (! (bool) v.getProperty ("ok", false)) return;
            auto arr = v.getProperty ("arreglos", juce::var());
            juce::MessageManager::callAsync ([sp, arr]
            {
                if (sp == nullptr) return;
                sp->songArrs.clear();
                if (auto* a = arr.getArray())
                    for (auto& e : *a)
                    {
                        const int id = (int) e.getProperty ("id", 0);
                        const juce::String nom = e.getProperty ("name", "").toString();
                        juce::Array<int> seq;
                        if (auto* s = e.getProperty ("seq", juce::var()).getArray())
                            for (auto& x : *s) seq.add ((int) x);
                        if (id > 0 && nom.isNotEmpty() && seq.size() > 0)
                        {
                            sp->songArrs[id].push_back ({ nom, seq });
                            if ((bool) e.getProperty ("last", false)) sp->songLastArr[id] = nom;   // arreglo activo
                        }
                    }
                sp->saveArr();     // refresca el caché local (offline)
                sp->repaint();
            });
        });
    }
    void pushArreglosServer()   // sube TODOS los arreglos de la org (reemplaza en el servidor)
    {
        if (serverUrl.isEmpty() || serverToken.isEmpty()) return;
        juce::Array<juce::var> a;
        for (auto& kv : songArrs)
        {
            const auto lit = songLastArr.find (kv.first);
            const juce::String last = (lit != songLastArr.end()) ? lit->second : juce::String();
            for (auto& ar : kv.second)
            {
                if (ar.second.size() == 0) continue;
                juce::DynamicObject::Ptr o = new juce::DynamicObject();
                o->setProperty ("id", kv.first);
                o->setProperty ("name", ar.first);
                juce::Array<juce::var> sq; for (int s : ar.second) sq.add (s);
                o->setProperty ("seq", sq);
                o->setProperty ("last", ar.first == last);   // arreglo activo de esa canción (lo sigue el chart del repertorio)
                a.add (juce::var (o.get()));
            }
        }
        const juce::String js = juce::JSON::toString (juce::var (a));
        const juce::String url = serverUrl + "/api/live/arreglos";
        const juce::String tok = serverToken;
        juce::Thread::launch ([url, js, tok]
        {
            juce::StringPairArray p; p.set ("arreglos", js);
            httpPostForm (url, p, tok);
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
            npTriggerLocalNetworkPermission();   // iOS: pedir permiso de Red local para el servidor entrante
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
        resized();   // aparece/desaparece la franja del indicador bajo el Play
        repaint();
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
       #if JUCE_IOS
        setup.bufferSize = 512;   // igual que el Mac (ver nota en el constructor): evita el desfase del conteo/saltos
       #else
        setup.bufferSize = 512;
       #endif
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
        audioCfg.setAutoPan (autoPan.load());
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
        root->setProperty ("autopan", autoPan.load());
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
        autoPan.store ((bool) v.getProperty ("autopan", false));
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
                sp->repaint();   // actualiza el indicador "NeuralSync conectado" bajo el Play
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
        if (editMode) initEditSeq();                  // preparar la secuencia del arreglo para editar
        updateTransportEnabled();
        keyMapBtn.setVisible (editMode);              // #4 botones de mapping solo en la barra de Editar
        midiMapBtn.setVisible (editMode);
        saveArrBtn.setVisible (editMode);             // "Guardar arreglo" solo en edición
        saveAsNewBtn.setVisible (editMode);           // "Guardar como nuevo" solo en edición
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
        const int realId = (songId < 0 ? -songId : songId);   // el placeholder "Agregando…" usa id NEGATIVO (-numero)
        for (int i = 0; i < repertoire.size(); ++i)   // UI optimista: la tarjeta desaparece al instante
        {
            const int rid = repertoire.getReference (i).id;
            if (rid == songId || rid == realId || rid == -realId)   // acepta placeholder o real
            {
                repertoire.remove (i);
                if (i < songMaster.size())   songMaster.remove (i);
                if (i < songMixCache.size()) songMixCache.remove (i);
                if (i < songReady.size())    songReady.remove (i);
                if (currentSong == i) { currentSong = -1; clearSong(); }
                else if (currentSong > i) --currentSong;
                break;
            }
        }
        dlById.erase (realId); dlById.erase (-realId);   // corta el indicador de "Descargando stems…"
        if (repertoire.isEmpty()) clearSong();           // sin canciones → mapping totalmente vacío
        rebuildRepertoireStrip();
        // SIEMPRE se quita por el id REAL (positivo), así el servidor lo borra aunque hayamos
        // tocado el placeholder; si no, al recargar la canción "revivía".
        postThenReload (serverUrl + "/api/live/setlist/" + lastSetlistId + "/quitar/" + juce::String (realId), {});
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
        // Diálogo con calendario (estilo app) en las 3 plataformas.
        datePrompt.onOk = [this] (juce::String nombre, juce::String fecha)
        { doCreateSetlist (nombre.isNotEmpty() ? nombre : juce::String ("Nuevo repertorio"), fecha); };
        datePrompt.setBounds (getLocalBounds());
        datePrompt.abrir (juce::String::fromUTF8 ("Nuevo repertorio"), "Crear", "");
    }

    void createSetlistAlertLegacy_unused()
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
        presentModalAlert (aw);
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

    // ── "Guardar como Nuevo": duplica el repertorio cargado (canciones + mezcla) ──
    void saveRepertoireAsNew()
    {
        if (serverUrl.isEmpty() || lastSetlistId.isEmpty()) return;
        juce::String baseName = "Repertorio";
        for (auto& it : repPicker.items) if (it.id == lastSetlistId) { baseName = it.nombre; break; }
        // Diálogo con calendario (estilo app) en las 3 plataformas.
        datePrompt.onOk = [this] (juce::String nombre, juce::String fecha)
        { duplicateCurrentSetlist (nombre.isNotEmpty() ? nombre : juce::String ("Repertorio (copia)"), fecha); };
        datePrompt.setBounds (getLocalBounds());
        datePrompt.abrir (juce::String::fromUTF8 ("Guardar como nuevo"), "Guardar",
                          baseName + juce::String::fromUTF8 (" (copia)"));
    }

    void saveRepertoireAsNewAlertLegacy_unused()
    {
        juce::String baseName = "Repertorio";
        const auto now = juce::Time::getCurrentTime();
        const auto hoy = juce::String::formatted ("%04d-%02d-%02d",
                                                  now.getYear(), now.getMonth() + 1, now.getDayOfMonth());
        auto* aw = new juce::AlertWindow (juce::String::fromUTF8 ("Guardar como nuevo"),
                                          juce::String::fromUTF8 ("Nombre y fecha del nuevo repertorio:"),
                                          juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("n", baseName + juce::String::fromUTF8 (" (copia)"), juce::String::fromUTF8 ("Nombre:"));
        aw->addTextEditor ("f", hoy, juce::String::fromUTF8 ("Fecha (AAAA-MM-DD):"));
        aw->addButton ("Guardar", 1);
        aw->addButton ("Cancelar", 0);
        presentModalAlert (aw);
        juce::Component::SafePointer<MainComponent> sp (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([sp, aw] (int r)
        {
            const juce::String nombre = aw->getTextEditorContents ("n").trim();
            const juce::String fecha  = aw->getTextEditorContents ("f").trim();
            if (r == 1 && sp != nullptr)
                sp->duplicateCurrentSetlist (nombre.isNotEmpty() ? nombre : juce::String ("Repertorio (copia)"), fecha);
        }), true);
    }

    void duplicateCurrentSetlist (juce::String nombre, juce::String fecha)
    {
        if (serverUrl.isEmpty() || lastSetlistId.isEmpty()) return;
        snapshotCurrentMix();
        struct SongCopy { int id; juce::String tono; juce::var mix; };
        juce::Array<SongCopy> songs;
        for (int i = 0; i < repertoire.size(); ++i)
        {
            auto& e = repertoire.getReference (i);
            if (e.id <= 0) continue;                                  // saltar placeholders
            SongCopy s; s.id = e.id; s.tono = e.tonoNombre;
            s.mix = (i < songMixCache.size()) ? songMixCache[i] : juce::var();
            songs.add (s);
        }
        const auto base = serverUrl; const auto tok = serverToken;
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, base, tok, nombre, fecha, songs]
        {
            juce::StringPairArray cp; cp.set ("nombre", nombre);
            if (fecha.isNotEmpty()) cp.set ("fecha", fecha);
            auto v = juce::JSON::parse (httpPostForm (base + "/api/live/setlist/crear", cp, tok));
            const auto nid = v.getProperty ("id", "").toString();
            if (nid.isEmpty()) return;
            juce::String data ("{"); bool first = true;
            for (auto& s : songs)
            {
                juce::StringPairArray ap; ap.set ("numero", juce::String (s.id)); ap.set ("tono", s.tono);
                httpPostForm (base + "/api/live/setlist/" + nid + "/agregar", ap, tok);   // copiar canción
                if (s.mix.isObject())
                {
                    if (! first) data << ",";
                    first = false;
                    data << "\"" << s.id << "\":" << juce::JSON::toString (s.mix);
                }
            }
            data << "}";
            juce::StringPairArray mp; mp.set ("data", data);
            httpPostForm (base + "/api/live/setlist/" + nid + "/mix", mp, tok);            // copiar mezcla
            juce::MessageManager::callAsync ([sp] { if (sp && sp->repPicker.isVisible()) sp->openRepertoirePicker(); });
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
        presentModalAlert (aw);
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

    // Aplica el tono elegido, cierra el panel (ya cerrado por el panel) y muestra
    // la barra de "preparando" en la PORTADA de la canción (no en una ventana).
    void prepararTono (int sem, juce::String nombre, bool yaListo)
    {
        const int sid = repEdit.songId; const bool add = repEdit.addFlow;
        // Tono ya listo (o el original): aplicar de una y listo.
        if (yaListo || sem == 0)
        {
            renderById.erase (sid);
            if (preparandoSongId == sid) preparandoSongId = -1;
            if (add) addSong (sid, nombre);
            else     setSongTono (sid, nombre);
            rebuildRepertoireStrip(); repaint();
            return;
        }
        // Aún NO está renderizado: mostrar barra en la portada y NO aplicar todavía
        // (así no se arrastra ni suena el tono viejo). Si es la canción cargada,
        // detenerla y marcar "Preparando" en el mapa; el tono se aplica al terminar.
        renderById[sid] = 0.02f;
        preparandoSongId = sid;
        const bool esActual = (currentSong >= 0 && currentSong < repertoire.size()
                               && repertoire.getReference (currentSong).id == sid);
        if (esActual) clearSong();   // detiene, descarga el audio viejo y limpia el mapa (mostrará "Preparando")
        rebuildRepertoireStrip(); repaint();
        const auto url = serverUrl, tok = serverToken;
        juce::Thread::launch ([url, tok, sid, sem]     // dispara el render (idempotente por .lock)
        { httpPostForm (url + "/api/live/render/" + juce::String (sid) + "/" + juce::String (sem), {}, tok); });
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, url, tok, sid, sem, add, nombre]
        {
            for (int it = 0; it < 900; ++it)          // ~10 min de tope
            {
                juce::Thread::sleep (700);
                auto est = juce::JSON::parse (httpGet (
                    url + "/api/live/render/" + juce::String (sid) + "/" + juce::String (sem) + "/estado", tok));
                const bool listo = (bool) est.getProperty ("listo", false);
                const auto prog = est.getProperty ("progreso", "").toString();
                float frac = 0.05f;
                if (prog.containsChar ('/'))
                {
                    const int h = prog.upToFirstOccurrenceOf ("/", false, false).getIntValue();
                    const int t = prog.fromFirstOccurrenceOf ("/", false, false).getIntValue();
                    if (t > 0) frac = juce::jlimit (0.05f, 0.99f, (float) h / (float) t);
                }
                if (listo)
                {
                    juce::MessageManager::callAsync ([sp, sid, add, nombre]
                    {
                        if (sp == nullptr) return;
                        sp->renderById.erase (sid);
                        if (sp->preparandoSongId == sid) sp->preparandoSongId = -1;
                        sp->pendingLoadId = sid;                  // auto-cargar esta canción cuando baje el tono nuevo
                        if (add) sp->addSong (sid, nombre);
                        else     sp->setSongTono (sid, nombre);   // recarga el setlist con el tono ya listo
                        sp->rebuildRepertoireStrip(); sp->repaint();
                    });
                    return;
                }
                juce::MessageManager::callAsync ([sp, sid, frac]
                {
                    if (sp == nullptr) return;
                    sp->renderById[sid] = frac;
                    sp->rebuildRepertoireStrip(); sp->repaint();
                });
            }
            juce::MessageManager::callAsync ([sp, sid]
            {
                if (sp == nullptr) return;
                sp->renderById.erase (sid);
                if (sp->preparandoSongId == sid) sp->preparandoSongId = -1;
                sp->rebuildRepertoireStrip(); sp->repaint();
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
                // Tono actual de la canción en el repertorio (si ya está agregada).
                int curSem = 0;
                if (! addFlow)
                    for (int i = 0; i < sp->repertoire.size(); ++i)
                        if (sp->repertoire.getReference (i).id == songId) { curSem = sp->repertoire.getReference (i).tono; break; }
                // Arreglos guardados de esta canción + cuál está activo (para la caja "Arreglo")
                const juce::StringArray arrs = sp->arrNamesFor (songId);
                int curArrIdx = 0;   // 0 = Original
                { auto lit = sp->songLastArr.find (songId);
                  if (lit != sp->songLastArr.end() && lit->second.isNotEmpty())
                      curArrIdx = arrs.indexOf (lit->second) + 1; }   // +1 por "Original"
                sp->repEdit.openTono (songId, title, addFlow, ks, i2, o2, pIntro, pOutro, curSem, arrs, juce::jmax (0, curArrIdx), sp->guiaSelFor (songId), sp->repTrackFor (songId));
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
    // Arreglos por canción (varios, con nombre) — persistencia local por id
    void loadArr()
    {
        songArrs.clear(); songLastArr.clear();
        auto v = juce::JSON::parse (npAppDir().getChildFile ("arreglos.json"));
        if (auto* a = v.getArray())
            for (auto& e : *a)
            {
                const int id = (int) e.getProperty ("id", 0);
                if (id <= 0) continue;
                juce::Array<int> seq;
                if (auto* sa = e.getProperty ("seq", juce::var()).getArray())
                    for (auto& s : *sa) seq.add ((int) s);
                const juce::String name = e.getProperty ("name", "").toString();
                if (seq.size() > 0 && name.isNotEmpty()) songArrs[id].push_back ({ name, seq });
                if ((bool) e.getProperty ("last", false)) songLastArr[id] = name;
            }
    }
    void saveArr()
    {
        juce::Array<juce::var> a;
        for (auto& kv : songArrs)
        {
            const auto lit = songLastArr.find (kv.first);
            const juce::String last = (lit != songLastArr.end()) ? lit->second : juce::String();
            for (auto& ar : kv.second)
            {
                if (ar.second.size() == 0) continue;
                juce::DynamicObject::Ptr o = new juce::DynamicObject();
                o->setProperty ("id", kv.first);
                o->setProperty ("name", ar.first);
                juce::Array<juce::var> sq; for (int s : ar.second) sq.add (s);
                o->setProperty ("seq", sq);
                o->setProperty ("last", ar.first == last);
                a.add (juce::var (o.get()));
            }
        }
        npAppDir().getChildFile ("arreglos.json").replaceWithText (juce::JSON::toString (juce::var (a)));
    }
    // ── Guía por canción (Tracks / Neural), persistente en guia_sel.json ──
    void loadGuiaSel()
    {
        songGuia.clear(); songRepTrack.clear();
        auto v = juce::JSON::parse (npAppDir().getChildFile ("guia_sel.json"));
        if (auto* a = v.getArray())
            for (auto& e : *a)
            {
                const int id = (int) e.getProperty ("id", 0);
                if (id > 0)
                {
                    songGuia[id] = (int) e.getProperty ("guia", 0) == 1 ? 1 : 0;
                    if ((bool) e.getProperty ("rep", false)) songRepTrack.insert (id);
                }
            }
    }
    void saveGuiaSel()
    {
        juce::Array<juce::var> a;
        std::set<int> ids;
        for (auto& kv : songGuia) ids.insert (kv.first);
        for (int id : songRepTrack) ids.insert (id);
        for (int id : ids)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("id", id);
            auto g = songGuia.find (id);
            o->setProperty ("guia", g != songGuia.end() ? g->second : 0);
            o->setProperty ("rep", songRepTrack.count (id) > 0);
            a.add (juce::var (o.get()));
        }
        npAppDir().getChildFile ("guia_sel.json").replaceWithText (juce::JSON::toString (juce::var (a)));
    }
    // ── Transición "al terminar" por REPERTORIO + canción, persistente en transiciones.json ──
    //    estructura: { setlistId: { songId: modo } }  modo 1=Autolink, 2=Sobreponer
    void loadTrans()
    {
        transPorSetlist.clear();
        auto v = juce::JSON::parse (npAppDir().getChildFile ("transiciones.json"));
        if (auto* a = v.getArray())
            for (auto& e : *a)
            {
                const juce::String setl = e.getProperty ("setlist", "").toString();
                const int sid  = (int) e.getProperty ("id", 0);
                const int modo = (int) e.getProperty ("modo", 0);
                if (setl.isNotEmpty() && sid > 0 && (modo == 1 || modo == 2))
                    transPorSetlist[setl][sid] = modo;
            }
    }
    void saveTrans()
    {
        juce::Array<juce::var> a;
        for (auto& kv : transPorSetlist)
            for (auto& sg : kv.second)
            {
                if (sg.second != 1 && sg.second != 2) continue;
                juce::DynamicObject::Ptr o = new juce::DynamicObject();
                o->setProperty ("setlist", kv.first);
                o->setProperty ("id", sg.first);
                o->setProperty ("modo", sg.second);
                a.add (juce::var (o.get()));
            }
        npAppDir().getChildFile ("transiciones.json").replaceWithText (juce::JSON::toString (juce::var (a)));
    }
    int transSelFor (int sid) const   // modo "al terminar" guardado para (setlist actual, canción); 0 = ninguno
    {
        if (lastSetlistId.isEmpty()) return 0;
        auto s = transPorSetlist.find (lastSetlistId);
        if (s == transPorSetlist.end()) return 0;
        auto it = s->second.find (sid);
        return (it != s->second.end()) ? it->second : 0;
    }
    void setTransActual (int modo)   // guarda/borra el modo "al terminar" para la canción actual en el setlist actual
    {
        autoTrans.store (modo);
        if (lastSetlistId.isEmpty() || currentSong < 0 || currentSong >= repertoire.size()) return;
        const int sid = repertoire.getReference (currentSong).id;
        if (modo == 1 || modo == 2) transPorSetlist[lastSetlistId][sid] = modo;
        else                        transPorSetlist[lastSetlistId].erase (sid);
        saveTrans();
    }
    int guiaSelFor (int sid) const   // 0 = Tracks (por defecto), 1 = Neural
    { auto it = songGuia.find (sid); return (it != songGuia.end()) ? it->second : 0; }
    bool repTrackFor (int sid) const   // ¿permitir repetir/∞ en modo Track para esta canción?
    { return songRepTrack.count (sid) > 0; }
    // Deja guideOn en el valor correcto para el estado actual: un arreglo activo SIEMPRE usa Neural;
    // en Original se respeta la elección de la canción (songGuia). Rearma el cue si se enciende.
    void aplicarGuiaSegunEstado()
    {
        const int sid = currentSongId();
        const bool neural = arrangementActive.load() ? true : (sid > 0 && guiaSelFor (sid) == 1);
        repetirEnTrack = (sid > 0 && repTrackFor (sid));   // permiso de repetir/∞ en modo Track
        const bool antes = guideOn.load();
        guideOn.store (neural);
        cueSchedStep = -1;
        if (neural) { computeGuiaRoute(); if (! antes) fetchGuideClips(); }
        updateNeuralCtrls();
    }
    bool repetirPermitido() const { return guideOn.load() || repetirEnTrack; }
    // Atenúa/deshabilita repetir y ∞ salvo en Neural (o si la canción habilitó repetir en Track).
    // Las flechas ‹ › quedan siempre activas: son navegación por sección en ambos modos.
    void updateNeuralCtrls()
    {
        const bool on = repetirPermitido();
        for (auto* b : { (juce::Component*) &repeatBtn, (juce::Component*) &infiniteBtn })
        {
            b->setAlpha (on ? 1.0f : 0.3f);
            b->setInterceptsMouseClicks (on, on);
            b->repaint();
        }
    }
    // nombres de arreglos guardados de una canción (para la caja "Arreglo" del diálogo)
    juce::StringArray arrNamesFor (int sid) const
    {
        juce::StringArray out;
        auto it = songArrs.find (sid);
        if (it != songArrs.end()) for (auto& ar : it->second) out.add (ar.first);
        return out;
    }
    // Aplica el arreglo elegido (del diálogo) a una canción. "" / "Original" = sin arreglo.
    // Si la canción YA está cargada, lo aplica en vivo al instante (porque la recarga del
    // setlist no vuelve a cargar una canción que ya es la actual).
    void aplicarArregloASong (int sid, juce::String arrName)
    {
        const bool esOriginal = (arrName.isEmpty() || arrName == juce::String::fromUTF8 ("Original"));
        songLastArr[sid] = esOriginal ? juce::String() : arrName;   // recordar la elección ("" = Original)
        saveArr();
        const bool esActual = (currentSong >= 0 && currentSong < repertoire.size()
                               && repertoire.getReference (currentSong).id == sid);
        if (! esActual) return;
        if (esOriginal) { arrLoadedName.clear(); clearArrangement(); }
        else
        {
            bool aplicado = false;
            auto it = songArrs.find (sid);
            if (it != songArrs.end())
                for (auto& ar : it->second)
                    if (ar.first == arrName && ar.second.size() > 0)
                    { arrLoadedName = ar.first; setArrangement (ar.second, ar.first, false); aplicado = true; break; }
            if (! aplicado) { arrLoadedName.clear(); clearArrangement(); }
        }
        if (editMode) initEditSeq();
        pushArreglosServer();   // subir el flag "activo" para que el repertorio (app) siga el arreglo correcto
        rebuildRepertoireStrip();   // refrescar el chip del arreglo en la tarjeta
        repaint();
    }
    // Borra un arreglo guardado de una canción (local + servidor por reemplazo).
    void borrarArreglo (int sid, juce::String nombre)
    {
        auto it = songArrs.find (sid);
        if (it != songArrs.end())
        {
            auto& lst = it->second;
            for (size_t i = 0; i < lst.size(); ++i)
                if (lst[i].first == nombre) { lst.erase (lst.begin() + (long) i); break; }
            if (lst.empty()) songArrs.erase (it);
        }
        // si el borrado era el "último aplicado", volver a Original
        auto lit = songLastArr.find (sid);
        if (lit != songLastArr.end() && lit->second == nombre) songLastArr[sid] = juce::String();
        // si estaba activo en la canción cargada, quitarlo en vivo
        if (arrLoadedName == nombre && currentSong >= 0 && currentSong < repertoire.size()
            && repertoire.getReference (currentSong).id == sid)
        { arrLoadedName.clear(); clearArrangement(); if (editMode) initEditSeq(); }
        saveArr();
        pushArreglosServer();   // reemplaza la lista de la org en el servidor (deja fuera el borrado)
        rebuildRepertoireStrip();   // refrescar el chip del arreglo en la tarjeta
        repaint();
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
        // Si ya hay una descarga en curso, NO rechazar: encolar y bajarla a continuación.
        if (offlineLoader && offlineLoader->isThreadRunning())
        {
            if (id == offlineId || offlineQueue.contains (id)) return;   // ya se está bajando o ya está en cola
            offlineQueue.add (id);
            setPickerDlPct (id, 0);                                      // muestra que quedó pendiente
            connStatus.setText ("En cola (" + juce::String (offlineQueue.size()) + ") - pendiente", juce::dontSendNotification);
            return;
        }
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
        offlineLoader->onDone     = [sp] (juce::Array<SongEntry> songs)
        {
            if (! sp) return;
            // Solo marcar "listo" si TODAS las canciones quedaron realmente en caché.
            bool complete = true;
            for (auto& e : songs) if (! sp->cacheReady (e)) { complete = false; break; }
            for (auto& it : sp->repPicker.items)
                if (it.id == sp->offlineId) { it.dlPct = -1; it.cached = complete; break; }
            if (sp->repPicker.isVisible()) sp->repPicker.repaint();
            sp->connStatus.setText (complete
                ? juce::String::fromUTF8 ("Repertorio descargado \xe2\x9c\x93")
                : juce::String::fromUTF8 ("Descarga incompleta - revis\xc3\xa1 tu conexi\xc3\xb3n"),
                juce::dontSendNotification);
            sp->offlineId.clear(); sp->offlinePct = -1;
            sp->refreshStorageStats();
            // Arrancar la siguiente de la cola, si hay.
            if (! sp->offlineQueue.isEmpty())
            {
                const auto next = sp->offlineQueue[0];
                sp->offlineQueue.remove (0);
                sp->downloadRepertoireOffline (next);
            }
        };
        offlineLoader->startThread();
        connStatus.setText (juce::String::fromUTF8 ("Descargando repertorio para offline\xe2\x80\xa6"), juce::dontSendNotification);
    }

    void startLoadId (juce::String setlistId)
    {
        if (serverToken.isEmpty()) { connStatus.setText ("Falta servidor/token", juce::dontSendNotification); return; }
        // Evitar recargas duplicadas del MISMO repertorio (doble-tap tactil): si ya hay un
        // loader trabajando en ese id, ignorar. Asi no quedan dos hilos bajando los mismos
        // stems en paralelo (se autolibera cuando el hilo termina: isThreadRunning() == false).
        if (loader && loader->isThreadRunning() && setlistId == lastSetlistId)
            return;
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
        // Actualizar el % del placeholder central mientras aun no hay cancion cargada,
        // repintando solo cuando cambia el entero (para no saturar durante la descarga).
        if (currentSong < 0)
        {
            const int pct = (int) (f * 100.0);
            if (pct != lastDlPct) { lastDlPct = pct; repaint (mapBounds); }
        }
        if (f >= 1.0 && idx >= 0 && idx < songReady.size())
        {
            songReady.set (idx, true);
            if (pendingLoadId >= 0 && id == pendingLoadId && currentSong < 0)
            { pendingLoadId = -1; loadSong (idx); }          // auto-cargar la canción cuyo tono se acaba de preparar
            else if (idx == 0 && currentSong < 0) loadSong (0);   // apenas esté la 1a, cargarla
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
        if (currentSong < 0)
        {
            int li = 0;                          // por defecto la 1a; si hay una preparada, esa
            if (pendingLoadId >= 0)
                for (int i = 0; i < repertoire.size(); ++i)
                    if (repertoire.getReference (i).id == pendingLoadId) { li = i; pendingLoadId = -1; break; }
            loadSong (li);
        }
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
            autoReqPending.store (0);   // Fase D: limpiar pedido de transición al cambiar de canción
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
        arrangementActive.store (false);   // desactivar arreglo ANTES de limpiar (el hilo de audio lo chequea)
        arrStep.store (0); arrLoop.store (false);
        arrangement.clear(); arrangementName.clear();
        queuedSection.store (-1); queuedFromSec.store (-1); queuedStep.store (-1);   // Fase B: sin salto encolado al cambiar de canción
        sectionTimes.clear();
        sectionNames.clear();
        clickSecArmed.store (false); songHasClickSec = false;   // sin canción no hay bloque "Click ∞"
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
            { auto lit = songLastArr.find (s.id);   // chip del arreglo activo ("" = Original → sin chip)
              c->arrNombre = (lit != songLastArr.end()) ? lit->second : juce::String(); }
            c->editMode = editMode;
            c->index = i;
            c->songId = s.id;
            {   // barra ligada al id: primero el render de tono ("preparando"), luego la descarga
                auto rit = renderById.find (s.id);
                if (rit != renderById.end())
                    c->dlProgress = rit->second;
                else
                {
                    auto it = dlById.find (s.id);
                    if (it != dlById.end() && cacheReady (s)) { dlById.erase (it); it = dlById.end(); }
                    c->dlProgress = (it != dlById.end()) ? it->second : -1.0f;
                }
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
        files.addArray (folder.findChildFiles (juce::File::findFiles, false, "*.flac"));   // tonos transpuestos salen en FLAC
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
            bufferingSources.add (new juce::BufferingAudioSource (rs, readThread, false, 240000, 2));  // ~5s de pre-carga por stem (canciones largas/pesadas)
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
        browsing = false;       // al cambiar de canción, el mapa vuelve a seguir el inicio
        if (padEnabled.load() && padMode == 0) { padApplyTone(); updatePadUi(); }   // pad en Auto: re-afina a la tonalidad de la canción
        const auto& sng = repertoire.getReference (index);

        if (mixBuilder) { mixBuilder->stopThread (2000); mixBuilder = nullptr; }

        {
            const juce::ScopedLock sl (graphLock);
            loadingSong.store (true);
            playing.store (false);
            positionOut.store (0);
            seekTo.store (-1);
            autoReqPending.store (0);   // Fase D: limpiar pedido de transición al cambiar de canción
            resamplers.clear();
            bufferingSources.clear();
            readerSources.clear();
            fileRates.clear();
            trackNames.clear();
            trackServerFam.clear();
            stemFiles.clear();
            lengthSamples = 0;
            numTracks = 0;
            // RESET del arreglo al cambiar de canción (si no, se queda el de la canción anterior → se atora)
            arrangementActive.store (false);
            arrStep.store (0); arrLoop.store (false);
            arrangement.clear(); arrangementName.clear(); arrLoadedName.clear();
            editSeq.clearQuick();
            queuedSection.store (-1); queuedFromSec.store (-1); queuedStep.store (-1);

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

        // Aplicar el arreglo elegido (diálogo) o el último usado de esta canción. "" / "Original" = natural.
        {
            juce::String want = pendingArrName;            // elegido en el diálogo de carga
            pendingArrName.clear();
            if (want.isEmpty())                            // sin elección explícita: usar el último aplicado
            {
                auto lit = songLastArr.find (sng.id);
                if (lit != songLastArr.end()) want = lit->second;
            }
            arrLoadedName.clear();
            if (want.isNotEmpty() && want != juce::String::fromUTF8 ("Original"))
            {
                auto it = songArrs.find (sng.id);
                if (it != songArrs.end())
                    for (auto& ar : it->second)
                        if (ar.first == want && ar.second.size() > 0)
                        {
                            arrLoadedName = ar.first;
                            setArrangement (ar.second, ar.first, false);
                            break;
                        }
            }
            songLastArr[sng.id] = arrLoadedName;           // recordar lo aplicado ("" = Original)
        }
        if (editMode) initEditSeq();   // si estamos editando y cambiamos de canción, preparar su secuencia

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
        aplicarGuiaSegunEstado();                      // guía (Tracks/Neural) según canción/arreglo
        autoTrans.store (transSelFor (sng.id));        // transición "al terminar" guardada para esta canción en este repertorio
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
        computeGuiaRoute();   // guía dinámica: ubicar el track/ruta "Guía" para enrutar los cues de voz
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
            trackIsGuia[i]  = fam.equalsIgnoreCase (juce::String::fromUTF8 ("Gu\xc3\xad" "a"));   // pista de guía del usuario
            trackNoFade[i] = trackIsClick[i] || trackIsGuia[i];                         // Click/Guía no suben en el conteo
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
        const bool mapOnly = npEsIPhone() && phoneShowMap;   // iPhone en vista Mapa: ocultar todo lo demás
        const bool tracks = (faderView == 0) && ! mapOnly;
        const bool buses  = (faderView == 1) && ! mapOnly;
        const bool midi   = (faderView == 2) && ! mapOnly;
        const bool pad    = (faderView == 3) && ! mapOnly;
        faderViewport.setVisible (! mapOnly && faderView != 2 && faderView != 3);
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
        const bool mapOnly = npEsIPhone() && phoneShowMap;
        faderViewBtn.active   = (faderView == 0) && ! mapOnly;
        busesBtn.setColour     (juce::TextButton::buttonColourId, (faderView == 1 && ! mapOnly) ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff1f1f1f));
        padPlayerBtn.setColour (juce::TextButton::buttonColourId, (faderView == 3 && ! mapOnly) ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff1f1f1f));
        muteMidiBtn.setColour  (juce::TextButton::buttonColourId, (faderView == 2 && ! mapOnly) ? juce::Colour (0xff2E8BFF) : juce::Colour (0xff1f1f1f));
        faderViewBtn.repaint(); busesBtn.repaint(); padPlayerBtn.repaint(); muteMidiBtn.repaint();
        if (faderView == 1) refreshBusStates();
        if (faderView == 2) midiPanel.refreshPorts();
        if (faderView == 3) refreshPadPanel();
        updateFaderVisibility();
        if (faderView == 0 || faderView == 1) layoutFaderStrip();
        if (npEsIPhone()) { resized(); repaint(); }   // re-acomoda el escenario único
    }

    void rebuildMidiOuts()
    {
        const juce::ScopedLock sl (midiLock);
        flushMidiOffs();
        midiOuts.clear();
        cajaOut.clearQuick();
        if (! featMidi) return;   // plan Basico: no se envia MIDI
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
    void seekSection (int dir)   // saltar al inicio de la sección anterior/siguiente (navegación: ambos modos)
    {
        cueAnnounceLanded.store (true); // en Neural anuncia la sección donde se cae (en Track se ignora)
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
        const double total = seqTotalOf (mapSeq());   // largo del mapa (secuencia) en tiempo display
        const double win = juce::jmin (20.0, juce::jmax (4.0, total));
        // El centro (aguja) va en tiempo DISPLAY (de la secuencia) para alinear con el mapa dibujado
        const double center = browsing ? browseCenter : seqDisplayNow();
        // Aguja fija al inicio (borde izquierdo): el mapa se desplaza por debajo.
        // Igual para natural y arreglo (sin acote) para poder llegar hasta el final del mapa.
        juce::ignoreUnused (total);
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
        if (splashOn && juce::Time::getMillisecondCounter() - splashStart > 2000)
        {
            splashOn = false;
            splash.setVisible (false);
            if (serverToken.isEmpty()) mostrarLoginDialog();   // el login aparece DESPUÉS del splash (~2s)
        }
        // Fase D: transición automática pedida por el hilo de audio (al terminar la canción).
        if (const int req = autoReqPending.exchange (0))
            transicionSiguiente (req == 2);
        // Sesión única: latido cada ~15 s para detectar expulsión aunque la app esté quieta.
        if (serverToken.isNotEmpty() && serverSession.isNotEmpty())
        {
            static int npSessPingCtr = 0;
            if (++npSessPingCtr >= 900)
            {
                npSessPingCtr = 0;
                const juce::String url = serverUrl + "/api/auth/ping", tok = serverToken;
                juce::Thread::launch ([url, tok] { httpGet (url, tok); }); // dispara npCheckKick en 401
            }
        }
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
        // Auto-regreso a seguir la aguja tras un rato sin interacción (igual en vivo o parado).
        // El reloj arranca al SOLTAR el dedo (ver mouseUp), para dar tiempo a mirar adelante
        // y encolar el salto; luego el mapa vuelve a seguir la aguja solo — como en Mac.
        if (browsing && ! isDragging && (juce::Time::getMillisecondCounter() - lastInteractionMs > 1800))
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
        if (currentSong >= 0 && currentSong < repertoire.size()
            && repertoire.getReference (currentSong).id == preparandoSongId) return;   // preparando tono: no tocar el viejo
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

    int indiceSiguiente() const   // siguiente canción lista del setlist (-1 si no hay)
    {
        for (int i = currentSong + 1; i < repertoire.size(); ++i)
            if (i >= songReady.size() || songReady[i]) return i;
        return -1;
    }
    // Fase D — Transición a la siguiente canción. crossfade=false → Autolink (corte seco).
    void transicionSiguiente (bool crossfade)
    {
        const int next = indiceSiguiente();
        if (next < 0) return;   // no hay siguiente lista
        if (! crossfade)        // Autolink: corte seco
        {
            loadSong (next);
            seekSeconds (0.0);
            if (countInEnabled) prepararConteo();
            guideWasPlaying = false;                 // re-armar conteo/guía de voz de la nueva canción
            cueActiveBuf = -1; cuePos = 0; cuePendingIdx.store (-1);
            playing.store (true);
            playButton.setButtonText ("Pausa");
            return;
        }
        // Sobreponer (crossfade 2 compases): la canción actual pasa a la "cola" (fade-out)
        // y la siguiente arranca al frente a full. Sin cortes porque la cola se mezcla aparte.
        if (resamplers.isEmpty())
        { loadSong (next); seekSeconds (0.0); playing.store (true); playButton.setButtonText ("Pausa"); return; }
        const double barsSec = (bpm > 0.0) ? 2.0 * juce::jmax (1, beatsPerBar) * 60.0 / bpm : 3.0;
        {
            const juce::ScopedLock sl (graphLock);
            const juce::ScopedLock tl (tailLock);
            readerSources2.swapWith (readerSources);       // el grafo actual (saliente) va a la cola
            bufferingSources2.swapWith (bufferingSources);
            resamplers2.swapWith (resamplers);
            fileRates2.swapWith (fileRates);
            // Marcar las pistas de click de la saliente para NO sonarlas en la cola
            // (así el click viejo no choca con el click de la canción nueva).
            for (int i = 0; i < kMaxTracks; ++i) tailIsClick[i] = trackIsClick[i];
            tailFadeSec = barsSec;
            tailGainCur = 1.0;
            tailActive.store (true);
        }
        loadSong (next);          // carga la siguiente al frente (limpia el grafo viejo que quedó en 'resamplers')
        seekSeconds (0.0);
        if (countInEnabled) prepararConteo();
        guideWasPlaying = false;                 // re-armar conteo/guía de voz de la nueva canción
        cueActiveBuf = -1; cuePos = 0; cuePendingIdx.store (-1);
        playing.store (true);
        playButton.setButtonText ("Pausa");
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
        if (! repetirPermitido() && ! loopOnce.load()) return;   // repetir solo en Neural o si la canción lo habilitó en Track
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
    void avisoPlanInfinito()
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            juce::String::fromUTF8 ("Disponible en el plan Plus"),
            juce::String::fromUTF8 ("El Reproductor Infinito se habilita desde el plan Plus. Actualiz\xc3\xa1 tu plan para usarlo."));
    }
    void toggleLoopInfinite()   // loop infinito de la sección actual (o de la sección de click si estamos en ella)
    {
        if (! repetirPermitido() && ! loopActive.load() && ! clickLoopOn.load()) return;   // ∞ solo en Neural o si la canción lo habilitó en Track
        if (! featInfinito && ! loopActive.load() && ! clickLoopOn.load()) { avisoPlanInfinito(); return; }   // solo bloquea al ENCENDER
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

    // Volver al inicio (botón reiniciar y tecla kaReturn): con arreglo va al PRIMER paso
    void irAlInicio()
    {
        queuedStep.store (-1); queuedSection.store (-1); queuedFromSec.store (-1);   // cancelar saltos encolados
        if (arrangementActive.load() && arrangement.size() > 0)
        {
            seekSeconds (secStartOf (arrangement[0]));
            arrStep.store (0);
            browsing = false;
        }
        else seekSeconds (0.0);
    }

    // ── #4 mapping de teclado ──
    void doAct (int a)
    {
        switch (a)
        {
            case kaPlay:      togglePlay();        break;
            case kaReturn:    irAlInicio();        break;
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
    void avisoPlanMidi()
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            juce::String::fromUTF8 ("Disponible en el plan Plus"),
            juce::String::fromUTF8 ("La secci\xc3\xb3n MIDI se habilita desde el plan Plus. Actualiz\xc3\xa1 tu plan para usarla."));
    }
    void aplicarPlanMidi (bool permitido)   // gating segun el plan de la organizacion
    {
        featMidi = permitido;
        if (! featMidi) { midiMapMode = false; setFadersArmable (false); }
        rebuildMidiOuts();
        refreshMapButtons();
        resized();
        repaint();
    }
    // ── Gating de acceso a NeuralPlay por plan ──
    // El plan "sync" (para DAW) NO incluye NeuralPlay: dispara desde su propio DAW.
    bool planIncluyeNeuralPlay (const juce::var& fs)
    {
        return ! fs.isObject() || (bool) fs.getProperty ("neuralplay", true);
    }
    juce::String msgPlanSinNeuralPlay (const juce::var& fs)
    {
        const juce::String pn = fs.getProperty ("nombre", "").toString();
        return juce::String::fromUTF8 ("Tu plan ")
             + (pn.isNotEmpty() ? (juce::String::fromUTF8 ("\xc2\xab") + pn + juce::String::fromUTF8 ("\xc2\xbb "))
                                : juce::String())
             + juce::String::fromUTF8 ("no incluye NeuralPlay: ese plan dispara desde tu propio DAW. "
                                       "Us\xc3\xa1 NeuralCharts para los m\xc3\xbasicos.");
    }

    void aplicarPlan (const juce::var& fs)   // aplica TODAS las features del plan (MIDI, salidas, infinito)
    {
        if (! fs.isObject()) return;
        featSalidas  = npCapSalidas ((int) fs.getProperty ("salidas",  32));
        featInfinito = (bool) fs.getProperty ("infinito", true);
        audioCfg.maxChans = featSalidas;
        audioCfg.buildRouteItems (openOutChans);   // re-limita el selector de salidas
        aplicarPlanMidi ((bool) fs.getProperty ("midi", true));   // llama a rebuild/resized/repaint
    }
    void toggleMidiMapMode()
    {
        if (! midiMapMode && ! featMidi) { avisoPlanMidi(); return; }   // MIDI solo desde plan Plus
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
        fadeButton.flipped = fadedDown;   // voltear el icono mientras está bajando/abajo
        fadeButton.repaint();
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
        seekToDisplay (vs + frac * (ve - vs));   // display -> real (y fija el paso)
    }
    void seekSeconds (double sec)
    {
        countInActive.store (false);   // #1 cualquier seek manual cancela el conteo
        const double fr = fileRates.isEmpty() ? 44100.0 : fileRates[0];
        seekTo.store ((long long) (juce::jmax (0.0, sec) * fr));
        // Si hay arreglo activo, recomputar en qué paso caímos (la sección que contiene 'sec').
        if (arrangementActive.load() && arrangement.size() > 0)
        {
            const int curSec = liveSectionIndexAt (juce::jmax (0.0, sec));
            int best = arrStep.load();
            if (best < 0 || best >= arrangement.size() || arrangement[best] != curSec)
            {
                best = -1;
                for (int k = 0; k < arrangement.size(); ++k)
                    if (arrangement[k] == curSec) { best = k; break; }
            }
            if (best >= 0) arrStep.store (best);
        }
        repaint (mapBounds);
    }

    // ── Arreglo de la canción (Fase A): activar / limpiar ──
    // 'orden' = índices de sección en el orden deseado. Se posiciona en la 1ª sección.
    void setArrangement (const juce::Array<int>& orden, const juce::String& nombre, bool loop)
    {
        double firstStart = 0.0; bool hasFirst = false;
        {
            const juce::ScopedLock sl (graphLock);
            arrangement = orden;
            arrangementName = nombre;
            arrLoop.store (loop);
            arrStep.store (0);
            arrangementActive.store (orden.size() > 0);
            if (orden.size() > 0) { firstStart = secStartOf (orden[0]); hasFirst = true; }
        }
        liveSongVer.fetch_add (1);   // avisar a los músicos que el arreglo cambió (chart lo sigue)
        aplicarGuiaSegunEstado();    // un arreglo activo fuerza la guía Neural
        if (hasFirst) seekSeconds (firstStart);
        repaint();
    }

    void clearArrangement()
    {
        arrangementActive.store (false);   // el hilo de audio lo chequea primero
        {
            const juce::ScopedLock sl (graphLock);
            arrangement.clear();
            arrangementName.clear();
            arrStep.store (0);
            arrLoop.store (false);
        }
        liveSongVer.fetch_add (1);   // avisar a los músicos (chart vuelve a la canción completa)
        aplicarGuiaSegunEstado();    // sin arreglo: volver a la guía elegida (Tracks/Neural)
        repaint();
    }

    // ── Helpers de rango de sección (soportan Conteo = kConteoIdx) ──
    double secStartOf (int idx) const
    {
        if (idx == kConteoIdx) return 0.0;
        if (idx >= 0 && idx < sectionTimes.size()) return sectionTimes[idx];
        return 0.0;
    }
    double secEndOf (int idx) const
    {
        if (idx == kConteoIdx) return sectionTimes.isEmpty() ? 0.0 : sectionTimes[0];
        if (idx >= 0 && idx < sectionTimes.size())
            return (idx + 1 < sectionTimes.size()) ? sectionTimes[idx + 1] : totalSeconds();
        return totalSeconds();
    }
    juce::String secNameOf (int idx) const
    {
        if (idx == kConteoIdx) return "Conteo";
        if (idx >= 0 && idx < sectionNames.size()) return sectionNames[idx];
        return "?";
    }
    bool hayConteo() const { return ! sectionTimes.isEmpty() && sectionTimes[0] > 0.4; }

    // ── Guía dinámica: slug del nombre de sección (DEBE coincidir con el del script Python) ──
    static juce::String slugSeccion (juce::String s)
    {
        s = s.toLowerCase().trim();
        s = s.replace (juce::String::fromUTF8 ("\xc3\xa1"), "a")   // á
             .replace (juce::String::fromUTF8 ("\xc3\xa9"), "e")   // é
             .replace (juce::String::fromUTF8 ("\xc3\xad"), "i")   // í
             .replace (juce::String::fromUTF8 ("\xc3\xb3"), "o")   // ó
             .replace (juce::String::fromUTF8 ("\xc3\xba"), "u")   // ú
             .replace (juce::String::fromUTF8 ("\xc3\xbc"), "u")   // ü
             .replace (juce::String::fromUTF8 ("\xc3\xb1"), "n");  // ñ
        juce::String out;
        for (auto ch : s)
        {
            if (ch == ' ') out << '_';
            else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') out << ch;
        }
        return out;
    }
    // Índice del clip de voz para una sección (o -1 si no hay). Conteo/Click/negativos = sin aviso.
    int guideClipForSection (int secIdx) const
    {
        if (secIdx < 0) return -1;
        const juce::String slug = slugSeccion (secNameOf (secIdx));
        auto it = guideSlugToIdx.find (slug);
        return (it != guideSlugToIdx.end()) ? it->second : -1;
    }
    // Localiza la pista "Guía" de la canción actual (para rutear/escalar el cue igual que ella).
    void computeGuiaRoute()
    {
        guiaTrkIdx = -1;
        for (int i = 0; i < numTracks && i < kMaxTracks; ++i)
        {
            const int fam = trackFamily[i];
            const juce::String fn = (fam >= 0 && fam < familyNames.size()) ? familyNames[fam] : juce::String();
            if (fn.equalsIgnoreCase (juce::String::fromUTF8 ("Gu\xc3\xad" "a")))
            { guiaTrkIdx = i; break; }
        }
    }
    // Arma el conteo hablado de 2 compases al inicio (uno,dos / uno,dos,tres,cuatro). Hilo de audio.
    // Usa la GRILLA real de beats para que caiga clavado sobre los tiempos (sin drift por BPM).
    void buildCountIn()
    {
        cntN = 0; cntIdx = 0;
        if (! guideOn.load() || ! guideLoaded.load()) return;
        if (sectionTimes.isEmpty()) return;
        const double songStart = sectionTimes[0];
        // Solo hay conteo si arrancamos ANTES del inicio de la canción (no al reanudar a mitad).
        const double posNow = (double) positionOut.load() / juce::jmax (1.0, deviceSampleRate);
        if (posNow >= songStart - 0.05) return;
        const int bpb  = juce::jmax (1, beatsPerBar);
        const double lead = 0.06;                    // disparar ~60 ms antes (ataque del clip)

        // Posiciones del conteo por TEMPO (negras), "pegadas" (snap) al beat de grilla más cercano.
        // Robusto ante grillas dañadas/dobladas (ej. Coritos quedó en corcheas): el tempo manda,
        // la grilla solo afina. Así el conteo cae bien aunque la grilla tenga el doble de beats.
        const double beatSec = (bpm > 0.0) ? 60.0 / bpm : 0.5;
        int expected = (int) std::llround (songStart / beatSec);   // negras que caben en el conteo
        expected = juce::jlimit (0, 2 * bpb, expected);            // como mucho 2 compases
        if (expected <= 0) return;
        std::vector<double> conteo;
        for (int k = expected; k >= 1; --k)
        {
            const double t = songStart - k * beatSec;
            if (t < -0.05) continue;
            double best = t, bestd = beatSec * 0.4;   // tolerancia de snap (< medio beat)
            for (int i = 0; i < currentBeatGrid.size(); ++i)
            {
                const double gb = currentBeatGrid[i];
                if (gb >= songStart) break;           // grilla ordenada; más allá no sirve
                const double dd = std::abs (gb - t);
                if (dd < bestd) { bestd = dd; best = gb; }
            }
            conteo.push_back (best);
        }
        const int win = (int) conteo.size();
        if (win <= 0) return;

        auto addClip = [&] (double t, int ci)
        {
            if (cntN >= 8 || ci < 0) return;
            cntAt[cntN] = (long long) (juce::jmax (0.0, t - lead) * deviceSampleRate);
            cntClip[cntN] = ci; ++cntN;
        };
        auto idxOf = [this] (const char* slug) -> int
        { auto it = guideSlugToIdx.find (juce::String (slug)); return it != guideSlugToIdx.end() ? it->second : -1; };

        // Primera sección real (nombre que irá en el tiempo 1 del compás del nombre).
        int firstSec = -999;
        if (arrangementActive.load() && arrangement.size() > 0)
        { for (int k = 0; k < arrangement.size(); ++k) if (arrangement[k] >= 0) { firstSec = arrangement[k]; break; } }
        else if (sectionTimes.size() > 0) firstSec = 0;
        const int clipFirst = (firstSec >= 0) ? guideClipForSection (firstSec) : -1;
        const char* seq[4] = { "conteo_1", "conteo_2", "conteo_3", "conteo_4" };

        if (win >= 2 * bpb)   // 2 compases completos: uno,dos | NOMBRE,dos,tres,cuatro
        {
            addClip (conteo[0],       idxOf ("conteo_1"));
            addClip (conteo[bpb / 2], idxOf ("conteo_2"));
            addClip (conteo[bpb],     clipFirst);
            for (int k = 1; k < bpb && k < 4; ++k) addClip (conteo[bpb + k], idxOf (seq[k]));
        }
        else   // 1 compás (o menos): NOMBRE en el 1er tiempo del último compás, luego 2,3,4
        {
            const int base = juce::jmax (0, win - bpb);   // inicio del último compás dentro del conteo
            addClip (conteo[base], clipFirst);
            for (int k = 1; base + k < win && k < 4; ++k) addClip (conteo[base + k], idxOf (seq[k]));
        }
    }
    // Descarga + decodifica la librería de clips de voz (una vez). Hilo de fondo.
    void fetchGuideClips()
    {
        if (serverUrl.isEmpty() || serverToken.isEmpty()) return;
        if (guideLoaded.load() || guideLoading.exchange (true)) return;
        const juce::String url = serverUrl, tok = serverToken;
        const double outSR = juce::jmax (8000.0, deviceSampleRate);
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, url, tok, outSR]
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
           #if JUCE_MAC || JUCE_IOS
            fm.registerFormat (new juce::CoreAudioFormat(), false);   // decodifica MP3 vía el SO
           #endif
            juce::File cacheDir = npAppDir().getChildFile ("guia_clips");
            cacheDir.createDirectory();

            auto v = juce::JSON::parse (httpGet (url + "/api/live/guia/list", tok));
            auto arr = v.getProperty ("clips", juce::var());
            juce::StringArray slugs;
            std::map<juce::String, juce::int64> srvBytes;   // tamaño del clip en el servidor (cache-busting)
            if (auto* a = arr.getArray())
                for (auto& e : *a)
                {
                    auto s = e.getProperty ("slug", "").toString();
                    if (s.isNotEmpty()) { slugs.add (s); srvBytes[s] = (juce::int64) e.getProperty ("bytes", 0); }
                }

            std::vector<juce::AudioBuffer<float>> bufs;
            std::map<juce::String, int> idx;
            for (auto& slug : slugs)
            {
                juce::File f = cacheDir.getChildFile (slug + ".mp3");
                const juce::int64 want = srvBytes.count (slug) ? srvBytes[slug] : 0;
                // Re-descarga si falta o si el tamaño del servidor cambió (clip regenerado).
                if (! f.existsAsFile() || f.getSize() <= 0 || (want > 0 && f.getSize() != want))
                    httpDownload (url + "/api/live/guia/" + slug + ".mp3", tok, f);
                if (! f.existsAsFile() || f.getSize() <= 0) continue;
                std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
                if (rd == nullptr || rd->lengthInSamples <= 0) continue;
                const int srcLen = (int) rd->lengthInSamples;
                juce::AudioBuffer<float> tmp ((int) juce::jmax ((juce::uint32) 1, rd->numChannels), srcLen);
                rd->read (&tmp, 0, srcLen, 0, true, true);
                juce::AudioBuffer<float> mono (1, srcLen);
                for (int n = 0; n < srcLen; ++n)
                {
                    float sum = 0.0f; for (int c = 0; c < tmp.getNumChannels(); ++c) sum += tmp.getSample (c, n);
                    mono.setSample (0, n, sum / juce::jmax (1, tmp.getNumChannels()));
                }
                const double ratio = rd->sampleRate / outSR;   // muestras fuente por muestra de salida
                if (ratio <= 0.0) continue;
                const int outLen = (int) std::ceil ((double) srcLen / ratio) + 2;
                juce::AudioBuffer<float> dst (1, outLen);
                dst.clear();
                juce::LagrangeInterpolator interp;
                interp.process (ratio, mono.getReadPointer (0), dst.getWritePointer (0), outLen);
                idx[slug] = (int) bufs.size();
                bufs.push_back (std::move (dst));
            }

            juce::MessageManager::callAsync ([sp, bufs = std::move (bufs), idx]() mutable
            {
                if (sp == nullptr) { return; }
                sp->guideBuffers = std::move (bufs);
                sp->guideSlugToIdx = idx;
                sp->guideLoaded.store (true);
                sp->guideLoading.store (false);
            });
        });
    }

    // ── Editor de arreglo (modo Editar): SECUENCIA ordenada (permite repetir/insertar/quitar) ──
    void initEditSeq()
    {
        editSeq.clearQuick();
        if (arrangementActive.load() && arrangement.size() > 0) { editSeq = arrangement; return; }
        if (hayConteo()) editSeq.add (kConteoIdx);
        for (int i = 0; i < sectionTimes.size(); ++i) editSeq.add (i);
    }
    void resetEditSeq()   // volver al mapa por defecto (todas en orden) y quitar arreglo
    {
        editSeq.clearQuick();
        if (hayConteo()) editSeq.add (kConteoIdx);
        for (int i = 0; i < sectionTimes.size(); ++i) editSeq.add (i);
        arrLoadedName.clear(); clearArrangement();
        const int sid = currentSongId();   // volver a "Original" (los arreglos guardados se conservan)
        if (sid > 0) { songLastArr[sid] = juce::String(); saveArr(); }
    }
    juce::Array<int> naturalSeq() const
    {
        juce::Array<int> s;
        if (hayConteo()) s.add (kConteoIdx);
        for (int i = 0; i < sectionTimes.size(); ++i) s.add (i);
        return s;
    }
    // Secuencia que se dibuja ahora: en Editar la que se edita; en vivo el arreglo activo; si no, natural.
    juce::Array<int> mapSeq() const
    {
        if (editMode) return editSeq;
        if (arrangementActive.load() && arrangement.size() > 0) return arrangement;
        return naturalSeq();
    }
    double seqDur (int idx) const { return juce::jmax (0.0, secEndOf (idx) - secStartOf (idx)); }
    double seqTotalOf (const juce::Array<int>& seq) const { double t = 0.0; for (int e : seq) t += seqDur (e); return t; }
    double seqDispStartOf (const juce::Array<int>& seq, int k) const
    { double t = 0.0; for (int i = 0; i < k && i < seq.size(); ++i) t += seqDur (seq[i]); return t; }
    // Posición de pantalla (display) del cursor de reproducción
    double seqDisplayNow() const
    {
        if (! arrangementActive.load()) return positionSeconds();   // natural: display == real
        auto seq = mapSeq(); if (seq.isEmpty()) return 0.0;
        const int st = juce::jlimit (0, seq.size() - 1, arrStep.load());
        const double off = juce::jlimit (0.0, seqDur (seq[st]), positionSeconds() - secStartOf (seq[st]));
        return seqDispStartOf (seq, st) + off;
    }
    // Display -> real (y devuelve el paso de la secuencia donde cae)
    double seqRealFromDisplay (double dt, int* outStep = nullptr) const
    {
        auto seq = mapSeq(); if (seq.isEmpty()) { if (outStep) *outStep = 0; return juce::jmax (0.0, dt); }
        double acc = 0.0;
        for (int k = 0; k < seq.size(); ++k)
        {
            const double d = seqDur (seq[k]);
            if (dt < acc + d || k == seq.size() - 1)
            { if (outStep) *outStep = k; return secStartOf (seq[k]) + juce::jlimit (0.0, d, dt - acc); }
            acc += d;
        }
        if (outStep) *outStep = seq.size() - 1;
        return juce::jmax (0.0, dt);
    }
    void seekToDisplay (double dt)
    {
        const juce::Array<int> seq = mapSeq();
        if (dt <= 0.05 && seq.size() > 0)   // al borde izquierdo: ir EXACTO al inicio del mapa (sin snap)
        {
            seekSeconds (secStartOf (seq[0]));
            if (arrangementActive.load()) arrStep.store (0);
            browsing = false;
            return;
        }
        int st = -1;
        const double real = seqRealFromDisplay (dt, &st);
        if (arrangementActive.load() && st >= 0)
        {
            // En arreglo el tiempo es por tramos: NO ajustar a beat (el snap podía caer en
            // otra sección real y "trabar" el deslizamiento). Caemos exacto en el display.
            seekSeconds (real);
            arrStep.store (st);
        }
        else
            seekSeconds (snapToBeat (real));
    }
    int currentSongId() const
    { return (currentSong >= 0 && currentSong < repertoire.size()) ? repertoire.getReference (currentSong).id : -1; }
    // Crea o SOBRESCRIBE (si ya existe el nombre) un arreglo de la canción actual
    void guardarSeqConNombre (const juce::String& nombre)
    {
        if (editSeq.size() == 0 || nombre.trim().isEmpty()) return;
        const juce::String nom = nombre.trim();
        const int sid = currentSongId();
        // Aviso: al usar una versión personalizada, la guía de tracks pasa a guía dinámica (una vez por canción/sesión).
        if (sid > 0 && guiaSelFor (sid) == 0 && guiaAvisada.insert (sid).second)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8 ("Gu\xc3\xad" "a din\xc3\xa1mica"),
                juce::String::fromUTF8 ("Est\xc3\xa1s usando una versi\xc3\xb3n personalizada de la canci\xc3\xb3n. "
                                        "Se cambiar\xc3\xa1 tu gu\xc3\xad" "a de track por una gu\xc3\xad" "a din\xc3\xa1mica."));
        arrLoadedName = nom;
        setArrangement (editSeq, nom, false);
        if (sid > 0)
        {
            auto& lst = songArrs[sid];
            bool found = false;
            for (auto& ar : lst) if (ar.first == nom) { ar.second = editSeq; found = true; break; }
            if (! found) lst.push_back ({ nom, editSeq });
            songLastArr[sid] = nom;
            saveArr();
            pushArreglosServer();   // sincronizar con la organización (servidor)
        }
    }
    void guardarArreglo()           // "Guardar arreglo": sobrescribe el arreglo cargado (si no hay, pide nombre)
    {
        if (arrLoadedName.isNotEmpty()) guardarSeqConNombre (arrLoadedName);
        else pedirNombreArreglo();
    }
    void guardarArregloComoNuevo() { pedirNombreArreglo (true); }   // siempre pide nombre y crea otro
    void cerrarNombreDlg()
    {
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::MessageManager::callAsync ([sp] { if (sp) sp->nombreDlg.reset(); });
    }
    void pedirNombreArreglo (bool comoNuevo = false)
    {
        struct Dlg : public juce::Component
        {
            juce::TextEditor field;
            juce::TextButton okBtn, cancelBtn;
            juce::String titulo { juce::String::fromUTF8 ("Guardar arreglo") };
            juce::String subt   { juce::String::fromUTF8 ("Pon\xc3\xa9 un nombre para este arreglo") };
            std::function<void (juce::String)> onOk;
            std::function<void ()> onClose;
            Dlg()
            {
                setAlwaysOnTop (true);
                field.setText ("Mi arreglo", false);
                field.setJustification (juce::Justification::centredLeft);
                field.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff1c1c1c));
                field.setColour (juce::TextEditor::textColourId, juce::Colours::white);
                field.setColour (juce::TextEditor::outlineColourId, juce::Colour (0x33ffffff));
                field.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0x662E6BE6));
                field.setIndents (12, 0);
                field.onReturnKey = [this] { confirmar(); };
                field.onEscapeKey = [this] { if (onClose) onClose(); };
                addAndMakeVisible (field);
                okBtn.setButtonText (juce::String::fromUTF8 ("Guardar"));
                okBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2E6BE6));
                okBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
                okBtn.onClick = [this] { confirmar(); };
                addAndMakeVisible (okBtn);
                cancelBtn.setButtonText (juce::String::fromUTF8 ("Cancelar"));
                cancelBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2a2a));
                cancelBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe6e6e6));
                cancelBtn.onClick = [this] { if (onClose) onClose(); };
                addAndMakeVisible (cancelBtn);
            }
            juce::Rectangle<int> panel() const { return getLocalBounds().withSizeKeepingCentre (380, 208); }
            void paint (juce::Graphics& g) override
            {
                g.fillAll (juce::Colour (0xB0000000));
                auto p = panel().toFloat();
                g.setColour (juce::Colour (0xff141414)); g.fillRoundedRectangle (p, 16.0f);
                g.setColour (juce::Colour (0x33ffffff)); g.drawRoundedRectangle (p, 16.0f, 1.2f);
                auto t = panel();
                g.setColour (juce::Colours::white); g.setFont (juce::Font (17.0f, juce::Font::bold));
                g.drawText (titulo, t.getX(), t.getY() + 20, t.getWidth(), 24, juce::Justification::centred);
                g.setColour (juce::Colour (0xffa3a3a3)); g.setFont (juce::Font (13.0f));
                g.drawText (subt, t.getX() + 20, t.getY() + 48, t.getWidth() - 40, 18, juce::Justification::centred);
                g.setColour (juce::Colour (0xff9aa0a6)); g.setFont (juce::Font (11.5f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("NOMBRE"), fieldRect().getX(), fieldRect().getY() - 18, 120, 15,
                            juce::Justification::centredLeft);
            }
            juce::Rectangle<int> fieldRect() const
            {
                auto p = panel(); return juce::Rectangle<int> (p.getX() + 40, p.getY() + 96, p.getWidth() - 80, 38);
            }
            void resized() override
            {
                field.setBounds (fieldRect());
                auto p = panel();
                const int bw = (p.getWidth() - 80 - 14) / 2, by = p.getBottom() - 56, bh = 40;
                okBtn.setBounds (p.getX() + 40, by, bw, bh);
                cancelBtn.setBounds (p.getX() + 40 + bw + 14, by, bw, bh);
            }
            void mouseDown (const juce::MouseEvent& e) override { if (! panel().contains (e.getPosition()) && onClose) onClose(); }
            void confirmar() { const auto nom = field.getText().trim(); if (nom.isNotEmpty() && onOk) onOk (nom); if (onClose) onClose(); }
            void enfocar() { field.grabKeyboardFocus(); field.selectAll(); }
        };

        auto* d = new Dlg();
        d->field.setText (dlgInicial.isNotEmpty() ? dlgInicial : juce::String ("Mi arreglo"), false);
        if (dlgTitulo.isNotEmpty()) d->titulo = dlgTitulo;
        else if (comoNuevo)         d->titulo = juce::String::fromUTF8 ("Guardar como nuevo");
        if (dlgSubt.isNotEmpty())   d->subt = dlgSubt;
        else if (comoNuevo)         d->subt = juce::String::fromUTF8 ("Pon\xc3\xa9 un nombre para el nuevo arreglo");
        if (dlgOkTxt.isNotEmpty())  d->okBtn.setButtonText (dlgOkTxt);
        juce::Component::SafePointer<MainComponent> sp (this);
        auto cb = dlgOnOk;   // si hay callback custom (renombrar), usarlo; si no, guardar normal
        d->onOk    = [sp, cb] (juce::String nom) { if (sp) { if (cb) cb (nom); else sp->guardarSeqConNombre (nom); sp->repaint(); } };
        d->onClose = [sp] { if (sp) sp->cerrarNombreDlg(); };
        nombreDlg.reset (d);
        addAndMakeVisible (d);
        d->setBounds (getLocalBounds());
        d->toFront (true);
        d->enfocar();
        // limpiar los parámetros de un solo uso
        dlgTitulo.clear(); dlgSubt.clear(); dlgInicial.clear(); dlgOkTxt.clear(); dlgOnOk = nullptr;
    }
    // Diálogo para RENOMBRAR un arreglo (mismo estilo, con el nombre actual precargado).
    void pedirRenombrarArreglo (int sid, juce::String viejo)
    {
        dlgTitulo  = juce::String::fromUTF8 ("Renombrar arreglo");
        dlgSubt    = juce::String::fromUTF8 ("Nuevo nombre para el arreglo");
        dlgInicial = viejo;
        dlgOkTxt   = juce::String::fromUTF8 ("Renombrar");
        dlgOnOk    = [this, sid, viejo] (juce::String nuevo) { renombrarArreglo (sid, viejo, nuevo); };
        pedirNombreArreglo();
    }
    void renombrarArreglo (int sid, juce::String viejo, juce::String nuevo)
    {
        nuevo = nuevo.trim();
        if (nuevo.isEmpty() || nuevo == viejo) return;
        auto it = songArrs.find (sid);
        if (it != songArrs.end())
        {
            for (auto& ar : it->second) if (ar.first == nuevo) return;   // evitar nombre duplicado
            for (auto& ar : it->second) if (ar.first == viejo) { ar.first = nuevo; break; }
        }
        auto lit = songLastArr.find (sid);
        if (lit != songLastArr.end() && lit->second == viejo) songLastArr[sid] = nuevo;
        if (arrLoadedName == viejo) { arrLoadedName = nuevo; arrangementName = nuevo; }
        saveArr();
        pushArreglosServer();
        rebuildRepertoireStrip();
        // refrescar la caja "Arreglo" del diálogo si está abierto para esa canción
        if (repEdit.isVisible() && repEdit.songId == sid)
        {
            juce::String selName = repEdit.selectedArrName();
            if (selName == viejo) selName = nuevo;
            repEdit.arrNames = arrNamesFor (sid);
            repEdit.selArrIdx = (selName == juce::String::fromUTF8 ("Original"))
                                  ? 0 : repEdit.arrNames.indexOf (selName) + 1;
            repEdit.repaint();
        }
        repaint();
    }
    // + : lista "Insertar sección" DESPUÉS del slot k — altura FIJA con scroll (permite repetir cualquier sección)
    void mostrarMenuInsertar (int k)
    {
        struct Picker : public juce::Component, public juce::ListBoxModel
        {
            juce::ListBox list { "secpick", nullptr };
            juce::StringArray items;
            juce::Array<int> ids;
            std::function<void (int)> onPick;
            Picker()
            {
                list.setModel (this);
                list.setRowHeight (30);
                list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff202329));
                list.setColour (juce::ListBox::outlineColourId, juce::Colour (0x33ffffff));
                list.setOutlineThickness (1);
                addAndMakeVisible (list);
            }
            int getNumRows() override { return items.size(); }
            void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
            {
                if (row < 0 || row >= items.size()) return;
                if (sel) { g.setColour (juce::Colour (0x552E6BE6)); g.fillRect (0, 0, w, h); }
                g.setColour (row == items.size() - 1 ? juce::Colour (0xff9aa0a6) : juce::Colours::white);
                g.setFont (juce::Font (14.0f));
                g.drawText (items[row], 12, 0, w - 16, h, juce::Justification::centredLeft);
                g.setColour (juce::Colour (0x12ffffff));
                g.fillRect (0, h - 1, w, 1);
            }
            void listBoxItemClicked (int row, const juce::MouseEvent&) override
            {
                if (row >= 0 && row < ids.size() && onPick) onPick (ids[row]);
            }
            void resized() override { list.setBounds (getLocalBounds()); }
        };

        auto* p = new Picker();
        if (hayConteo()) { p->items.add (juce::String::fromUTF8 ("Cuenta Inicial")); p->ids.add (kConteoIdx); }
        // Nombres de sección (usa sectionNames; si faltara alguno, cae a "Sección N")
        const int nSec = juce::jmax (sectionNames.size(), sectionTimes.size());
        for (int i = 0; i < nSec; ++i)
        {
            const juce::String nm = (i < sectionNames.size() && sectionNames[i].isNotEmpty())
                                      ? sectionNames[i]
                                      : (juce::String::fromUTF8 ("Secci\xc3\xb3n ") + juce::String (i + 1));
            p->items.add (nm); p->ids.add (i);
        }
        p->items.add (juce::String::fromUTF8 ("Restablecer el mapa por defecto")); p->ids.add (9999);
        p->list.updateContent();   // IMPRESCINDIBLE: refrescar el nº de filas tras poblar items
        p->setSize (190, 250);   // altura FIJA (~8 filas visibles); el resto se ve con scroll

        juce::Component::SafePointer<MainComponent> sp (this);
        Picker* pp = p;
        p->onPick = [sp, k, pp] (int id)
        {
            if (sp != nullptr)
            {
                if (id == 9999) sp->resetEditSeq();
                else { const int pos = juce::jlimit (0, sp->editSeq.size(), k + 1); sp->editSeq.insert (pos, id); }
                sp->repaint();
            }
            if (auto* cob = pp->findParentComponentOfClass<juce::CallOutBox>()) cob->dismiss();
        };
        const auto mp = juce::Desktop::getInstance().getMousePosition();
        juce::CallOutBox::launchAsynchronously (std::unique_ptr<juce::Component> (p),
                                                juce::Rectangle<int> (mp.x, mp.y, 1, 1), nullptr);
    }
    // Dibuja el arreglo como la forma de onda COMPACTADA (secciones pegadas, sin huecos) con − y +
    // Dibuja el mapping como el ARREGLO: solo las secciones del arreglo, pegadas (sin huecos),
    // con la forma de onda de cada una y pills de nombre — igual look que el mapa normal.
    // editable = modo Editar (muestra − y + chiquitos). Guarda el layout para el seek/playhead.
    void drawArrMap (juce::Graphics& g, juce::Rectangle<int> inner, const juce::Array<int>& seq, bool editable)
    {
        arrSegRects.clearQuick(); arrSegSec.clearQuick();
        if (editable) { slotMinusRect.clear(); addChipRect = {}; }
        g.setColour (juce::Colour (0xff0a0a0a)); g.fillRect (inner);   // tapa la onda/bloques lineales de abajo
        const int n = seq.size();
        if (n <= 0)
        {
            g.setColour (juce::Colour (0xffb0b0b0)); g.setFont (juce::Font (13.0f));
            g.drawText (juce::String::fromUTF8 ("Arreglo vac\xc3\xado \xe2\x80\x94 tocá + para agregar secciones"), inner.withTrimmedRight (40), juce::Justification::centred);
            if (editable)   // + para volver a agregar
            {
                juce::Rectangle<int> add (inner.getRight() - 30, inner.getCentreY() - 13, 26, 26);
                g.setColour (juce::Colour (0xff2E6BE6)); g.fillEllipse (add.toFloat());
                g.setColour (juce::Colours::white); g.setFont (juce::Font (18.0f, juce::Font::bold));
                g.drawText ("+", add, juce::Justification::centred);
                addChipRect = add;
            }
            return;
        }
        double tot = 0.0;
        for (int k = 0; k < n; ++k) tot += juce::jmax (0.25, secEndOf (seq[k]) - secStartOf (seq[k]));
        if (tot <= 0.0) return;
        const int curStep = (! editable && arrangementActive.load()) ? arrStep.load() : -1;
        const double posNow = positionSeconds();
        int x = inner.getX();
        for (int k = 0; k < n; ++k)
        {
            const int s = seq[k];
            const bool isConteo = (s == kConteoIdx);
            const double d = juce::jmax (0.25, secEndOf (s) - secStartOf (s));
            int w = (k == n - 1) ? (inner.getRight() - x) : (int) std::round (d / tot * inner.getWidth());
            if (w < 6) w = 6;
            juce::Rectangle<int> seg (x, inner.getY(), w, inner.getHeight());
            arrSegRects.add (seg); arrSegSec.add (s);
            auto in2 = seg.reduced (1, 1);
            // onda del segmento
            if (! isConteo && thumb.getTotalLength() > 0.0)
            {
                g.setColour (juce::Colour (0xffced1d6));
                thumb.drawChannels (g, in2, secStartOf (s), secEndOf (s), 1.2f);
            }
            else if (isConteo)
            {
                g.setColour (juce::Colour (0x14ffd27f)); g.fillRect (in2);
            }
            // resaltar el paso que suena (playback)
            if (! editable && k == curStep) { g.setColour (juce::Colour (0x15ffffff)); g.fillRect (seg); }
            // divisoria
            if (k > 0) { g.setColour (juce::Colour (0x33ffffff)); g.fillRect (x - 1, inner.getY(), 2, inner.getHeight()); }
            // nombre (chip arriba-izquierda, igual que el mapa normal)
            {
                const juce::String nm = secNameOf (s);
                const float tw = juce::jmin ((float) seg.getWidth() - 8.0f, (float) nm.length() * 7.0f + 14.0f);
                juce::Rectangle<float> chip ((float) seg.getX() + 4.0f, (float) seg.getY() + 3.0f, juce::jmax (16.0f, tw), 18.0f);
                g.setColour (juce::Colour (0xcc0c0c0c)); g.fillRoundedRectangle (chip, 5.0f);
                g.setColour (juce::Colours::white); g.setFont (juce::Font (11.5f, juce::Font::bold));
                g.drawText (nm, chip.reduced (6.0f, 0.0f), juce::Justification::centredLeft, true);
            }
            // − quitar esta sección (solo en edición) — la quita y el resto se compacta
            if (editable)
            {
                const int r = 9;
                juce::Rectangle<int> minus (seg.getX() + 4, seg.getBottom() - 2 * r - 5, 2 * r, 2 * r);
                g.setColour (juce::Colour (0xffE5534B)); g.fillEllipse (minus.toFloat());
                g.setColour (juce::Colours::white); g.setFont (juce::Font (13.0f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("\xe2\x88\x92"), minus, juce::Justification::centred);
                slotMinusRect[s] = minus;
            }
            // playhead (playback): dentro del segmento que suena
            if (! editable && k == curStep && posNow >= secStartOf (s) - 0.02 && posNow <= secEndOf (s) + 0.05)
            {
                const float frac = (float) ((posNow - secStartOf (s)) / juce::jmax (0.05, secEndOf (s) - secStartOf (s)));
                const float px = seg.getX() + juce::jlimit (0.0f, 1.0f, frac) * seg.getWidth();
                g.setColour (juce::Colours::white.withAlpha (0.10f));
                g.fillRect (juce::Rectangle<float> ((float) seg.getX(), (float) seg.getY(), px - seg.getX(), (float) seg.getHeight()));
                g.setColour (juce::Colours::white);
                g.fillRect (px - 1.0f, (float) inner.getY(), 2.0f, (float) inner.getHeight());
            }
            x += w;
        }
    }
    juce::String serverUrl, serverToken, serverSession;
    juce::Array<SongEntry> repertoire;
    juce::Array<double> songMaster;   // master (dB) independiente por cancion
    juce::Array<juce::var> songMixCache;   // mezcla por cancion (del repertorio cargado)
    juce::Array<bool> songReady;      // audio de la canción ya descargado
    std::map<int, float> dlById;      // id de canción -> progreso 0..1 (ausente = sin barra). Sigue a la canción al reordenar
    std::map<int, float> renderById;  // id -> progreso del render de tono (barra "preparando" en la portada)
    int preparandoSongId = -1;        // canción cuyo NUEVO tono se está preparando (bloquea play y muestra aviso en el mapa)
    int pendingLoadId = -1;           // canción a cargar automáticamente cuando su nuevo tono termine de bajar
    int lastDlPct = -1;               // ultimo % mostrado en el placeholder de descarga (para repintar sin saturar)
    juce::Array<int> loadOrderIds;    // ids en el ORDEN del loader (fijo); mapea el índice del loader al id aunque se reordene
    int pendingAddAfterId = 0;   // botón + de la tarjeta: insertar la canción agregada justo después de esta (0 = al final)
    int currentSong = -1;
    std::unique_ptr<RepertoireLoader> loader;
    std::unique_ptr<RepertoireLoader> offlineLoader;   // descarga de un repertorio para offline (no cambia la UI)
    juce::String offlineId;                            // repertorio que se está bajando para offline
    int offlineTotal = 0, offlinePct = -1;
    juce::StringArray offlineQueue;                    // repertorios en espera (cola de descargas offline)

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbCache { 1 };
    juce::AudioThumbnail thumb { 256, formatManager, thumbCache };
    juce::TimeSliceThread readThread { "np-read" };
    juce::OwnedArray<juce::AudioFormatReaderSource> readerSources;
    juce::OwnedArray<juce::BufferingAudioSource> bufferingSources;
    juce::OwnedArray<juce::ResamplingAudioSource> resamplers;
    juce::Array<double> fileRates;
    // ── Fase D: "cola" de la canción saliente para el crossfade (Sobreponer) ──
    juce::OwnedArray<juce::AudioFormatReaderSource> readerSources2;
    juce::OwnedArray<juce::BufferingAudioSource> bufferingSources2;
    juce::OwnedArray<juce::ResamplingAudioSource> resamplers2;
    juce::Array<double> fileRates2;
    juce::CriticalSection tailLock;                // protege el segundo grafo (aparte de graphLock)
    std::atomic<bool> tailActive { false };        // hay cola sonando (fade-out)
    std::atomic<int>  autoTrans { 0 };             // transición armada al terminar: 0 off, 1 Autolink, 2 Sobreponer
    std::atomic<int>  autoReqPending { 0 };        // pedido del hilo de audio al timer (0 none, 1 autolink, 2 crossfade)
    double tailGainCur = 0.0;                       // ganancia actual de la cola (hilo de audio)
    double tailFadeSec = 3.0;                       // duración del fade-out (2 compases del saliente)
    juce::AudioBuffer<float> tailTemp;             // buffer temporal del tail (dimensionado en prepareToPlay)
    juce::StringArray trackNames;
    juce::StringArray trackServerFam;          // familia (del servidor) por track
    juce::StringArray curFamFiles, curFamNames;
    juce::Array<double> sectionTimes;
    juce::StringArray sectionNames;
    // ── Arreglo de la canción (Fase A) ──
    // arrangement = índices de sección en el orden a reproducir (ej: [0,2,2,3]).
    //   Cada valor indexa sectionTimes/sectionNames. Vacío/inactivo = orden lineal normal.
    //   Se lee/escribe bajo graphLock (igual que sectionTimes), seguro para el hilo de audio.
    juce::Array<int>  arrangement;
    std::atomic<bool> arrangementActive { false };
    std::atomic<int>  arrStep { 0 };          // paso actual dentro de arrangement (hilo audio <-> UI)
    std::atomic<bool> arrLoop { false };      // al terminar el arreglo, reiniciarlo
    juce::String      arrangementName;        // nombre del arreglo cargado (para la UI)
    // Editor de arreglo (modo Editar): línea de tiempo COMPACTADA del arreglo (sin huecos)
    static constexpr int kConteoIdx = -2;
    // Edición: SECUENCIA ordenada del arreglo (permite repetir/insertar/quitar secciones)
    juce::Array<int> editSeq;                                        // secuencia que se edita (índices de sección / kConteoIdx)
    std::map<int, juce::Rectangle<int>> slotMinusRect, slotPlusRect; // rects − (quitar) y + (insertar) por slot de la secuencia
    juce::Rectangle<int> addChipRect;                                // (sin uso con el modelo de secuencia; se mantiene por compat)
    // Reproducción con arreglo: mapping compactado (solo secciones del arreglo, pegadas)
    juce::Array<juce::Rectangle<int>> arrSegRects;                   // rect de cada segmento del arreglo dibujado (seek/tap)
    juce::Array<int> arrSegSec;                                      // sección (o kConteoIdx) de cada segmento dibujado
    juce::Array<juce::Rectangle<int>> arrChipRects;                  // rect del bloque de cada sección en vivo (tap = encolar salto)
    std::atomic<int>  queuedSection { -1 };                          // Fase B (natural): sección encolada para saltar al terminar la actual
    std::atomic<int>  queuedFromSec { -1 };                          // Fase B (natural): sección donde se encoló (se salta al terminar ESA)
    std::atomic<int>  queuedStep { -1 };                             // Fase B (arreglo): PASO encolado; salta al terminar la sección actual
    juce::String      arrLoadedName;                                 // nombre del arreglo cargado ("" = nuevo, aún sin guardar)
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
    int  pendingChipK = -1;                 // slot tocado en vivo (acción al soltar: tap=encolar, drag=deslizar)
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
    bool phoneShowMap = true;               // iPhone: escenario único → true = Mapa, false = Faders/vista
    int vuTick = 0;                         // para refrescar el VU a la mitad de FPS

    std::atomic<bool> loopActive { false };     // infinito (permanente)
    std::atomic<bool> loopOnce { false };       // repetir una vez
    std::atomic<double> loopStartSec { -1.0 };
    std::atomic<double> loopEndSec { -1.0 };
    // Anti-click en saltos de arreglo no contiguos: fundido de entrada corto en el destino.
    std::atomic<double> arrFadeStart { -1.0 };
    std::atomic<double> arrFadeEnd   { -1.0 };
    static constexpr double kArrFadeSec = 0.020;   // 20 ms

    // ── Guía dinámica: anuncia la próxima sección (voz) 1 compás antes, por la ruta/fader de "Guía" ──
    std::atomic<bool> guideOn { false };                       // toggle (persistente)
    std::vector<juce::AudioBuffer<float>> guideBuffers;        // clips mono @ deviceSampleRate (idx)
    std::map<juce::String, int> guideSlugToIdx;                // slug de sección -> índice de buffer
    std::atomic<bool> guideLoaded { false };
    std::atomic<bool> guideLoading { false };
    std::atomic<int>  cuePendingIdx { -1 };                    // clip a disparar (lo pone el timer)
    std::atomic<long long> cueFireAtSample { -1 };             // instante de disparo (muestras del dispositivo)
    int cueSchedStep = -1000;                                  // paso del arreglo ya programado (hilo timer)
    int linSchedSec  = -1000;                                  // sección ya programada en modo lineal
    bool guideWasPlaying = false;                              // detectar inicio de Play (anunciar 1ª sección)
    bool guideWasLoop = false;                                 // detectar entrada/salida de repetición (re-programar cue)
    std::atomic<bool> cueAnnounceLanded { false };             // salto inmediato: anunciar la sección donde se cae
    int cueActiveBuf = -1;                                     // clip sonando (hilo de audio)
    int cuePos = 0;
    int guiaTrkIdx = -1;                                       // pista "Guía" (ruta/fader) de la canción actual
    // Conteo hablado del inicio (2 compases: uno,dos / uno,dos,tres,cuatro)
    long long cntAt[8] = { 0 };                               // instante (muestras del dispositivo) de cada número
    int cntClip[8] = { -1 };                                  // índice de clip (conteo_1..4) de cada número
    int cntN = 0, cntIdx = 0;                                 // total y siguiente a disparar

    // Arreglos guardados por canción (varios, con nombre). Persistente en arreglos.json por id.
    std::map<int, std::vector<std::pair<juce::String, juce::Array<int>>>> songArrs;  // id -> [{nombre, secuencia}]
    std::map<int, juce::String> songLastArr;   // id -> último arreglo aplicado ("" = Original)
    std::map<int, int> songGuia;               // id -> guía elegida (0 = Tracks, 1 = Neural). Ausente = Tracks.
    std::set<int> songRepTrack;                // ids que habilitaron repetir/∞ en modo Track
    std::map<juce::String, std::map<int,int>> transPorSetlist;   // setlistId -> (songId -> modo 1/2) transición al terminar
    bool repetirEnTrack = false;               // la canción actual permite repetir/∞ en Track
    std::set<int> guiaAvisada;                 // ids ya avisados del cambio a guía dinámica (una vez por sesión)
    juce::String pendingArrName;               // arreglo elegido en el diálogo de carga, a aplicar en loadSong
    std::unique_ptr<juce::Component> nombreDlg;  // diálogo propio (redondeado) para nombrar el arreglo
    // Parámetros de un solo uso para reutilizar el diálogo de nombre (guardar / renombrar)
    juce::String dlgTitulo, dlgSubt, dlgInicial, dlgOkTxt;
    std::function<void (juce::String)> dlgOnOk;
    // #2 punto de inicio/fin por canción (-1 = sin definir)
    std::map<int, std::pair<double,double>> songInOut;   // id de canción -> {inicio, fin}
    std::atomic<double> songInSec  { -1.0 };
    std::atomic<double> songOutSec { -1.0 };

    // #3 sección de click: bloque de 2 compases DESPUÉS del final, con metrónomo sintetizado
    bool trackIsClick[kMaxTracks] = { false };
    bool tailIsClick[kMaxTracks] = { false };          // Fase D: pistas de click de la saliente (no suenan en la cola del crossfade)
    bool trackIsGuia[kMaxTracks]  = { false };          // pista "Guía" del usuario (se silencia con guía Neural)
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
    bool featMidi = true;                            // el plan habilita MIDI (Plus+); Basico = false
    int  featSalidas = 32;                           // salidas de audio permitidas (2 en Basico, 32 en Plus+)
    bool featInfinito = true;                         // boton Reproductor Infinito (Plus+); Basico = false
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
    juce::Image logoInternoImg;   // wordmark "NeuralPlay" para la esquina del header
    int hdrLogoX = 28, hdrLogoY = 16;   // posicion del logo en el header (se fija en resized, respeta safe area)
    std::unique_ptr<NeuralLoginOverlay> loginOverlay;   // login tactil (iOS/iPad)
    PillLNF pillLnf;
    FaderLNF faderLnf;
    juce::TextButton connectButton;
    SkipStartButton returnButton;                 // "ir al inicio" (icono vectorial)
    TriIconButton barPrevBtn, barNextBtn;         // flechas de navegacion por seccion (icono vectorial)
    PlayIconButton playButton;
    FadeIconButton fadeButton;
    SkipNextButton transBtn;                      // Fase D: transición en vivo a la siguiente canción (ícono vector)
    juce::Array<double> preFadeVals;
    int fadeDir = 0;            // -1 bajando, +1 subiendo, 0 quieto
    bool fadedDown = false;
    juce::Label connStatus, timeLabel, masterLabel;
    juce::Slider masterSlider;
    juce::TextButton busesBtn, padPlayerBtn, muteMidiBtn, editBtn, padBtn, keyMapBtn, saveArrBtn, saveAsNewBtn;
    IconButton faderViewBtn, repeatBtn, infiniteBtn, settingsBtn, repertoireBtn;
    FaderStripComp faderStrip;
    HScrollViewport faderViewport;
    MidiPanel midiPanel;
    RepertoirePicker repPicker;
    NPDatePrompt datePrompt;                 // diálogo con calendario (Mac) para crear/duplicar
    SettingsPanel settingsPanel;
    StoragePanel storagePanel;
    bool cacheAutoClean = false;
    int  cacheCapGB = 0;
    bool didStartupClean = false;
    juce::String lastSetlistId;   // setlist cargado (para "Actualizar")
    juce::String currentSetlistName;
    juce::Rectangle<int> syncBadgeBounds;   // franja del indicador "NeuralSync conectado" (bajo el Play)
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
    std::atomic<bool> autoPan { false };   // Autopan: Click/Guia -> canal derecho, resto -> izquierdo (jack 2 salidas)
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
           #if JUCE_IOS || JUCE_ANDROID
            setFullScreen (true);
           #else
            setResizable (true, true);
            // Respetar el área utilizable de la pantalla: la ventana no debe pasar por
            // debajo del Dock (barra de iconos) ni de la barra de menús.
            auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
            auto ua = (disp != nullptr) ? disp->userArea : juce::Rectangle<int> (0, 0, 1440, 860);
            const int maxW = juce::jmax (1040, ua.getWidth());
            const int maxH = juce::jmax (700,  ua.getHeight());   // tope = alto utilizable (arriba del Dock)
            setResizeLimits (juce::jmin (1040, maxW), juce::jmin (860, maxH), maxW, maxH);
            int w = juce::jmin (getWidth(),  maxW);
            int h = juce::jmin (getHeight(), maxH);
            setBounds (ua.getCentreX() - w / 2, ua.getY(), w, h);
           #endif
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

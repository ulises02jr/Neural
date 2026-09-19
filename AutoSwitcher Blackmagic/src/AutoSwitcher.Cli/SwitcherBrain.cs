namespace AutoSwitcher.Core;

// Un paso de la secuencia manual: camara + segundos + transicion.
public class SeqStep
{
    public long Camera { get; set; }
    public double Seconds { get; set; } = 5;
    public string Transition { get; set; } = "cut"; // "cut" | "mix"
    public double Fade { get; set; } = 1.0;
}

// FASE 3 - El "cerebro". Portado de su switcher_completo.py.
// Agnostico de marca: solo depende de ISwitcherDriver.
public sealed class SwitcherBrain
{
    private readonly ISwitcherDriver _driver;
    private readonly Random _rng = new();
    private readonly List<long> _lastCameras = new();

    public bool AutoMode { get; set; } = true;
    public int IntervalSeconds { get; set; } = 10;
    public List<long> AvailableCameras { get; set; }
    public long CurrentCamera { get; private set; }
    public string TransitionMode { get; set; } = "cut"; // "cut" | "mix"
    public double FadeDurationSeconds { get; set; } = 1.0;
    public bool SequenceMode { get; set; } = false;
    public List<SeqStep> Sequence { get; set; } = new();
    private int _seqIndex = -1;
    public int SwitchCount { get; private set; }
    public DateTime StartTime { get; } = DateTime.Now;
    public DateTime NextSwitchTime { get; private set; }

    public SwitcherBrain(ISwitcherDriver driver)
    {
        _driver = driver;
        AvailableCameras = driver.Cameras.Select(c => c.Id).ToList();
        CurrentCamera = driver.ProgramInput;
        NextSwitchTime = DateTime.Now.AddSeconds(IntervalSeconds);
    }

    // Algoritmo anti-repeticion (identico a get_next_camera de Python):
    // evita la actual y las ultimas 2, elige al azar el resto.
    public long GetNextCamera()
    {
        var available = AvailableCameras.ToList();
        if (available.Count == 0) return CurrentCamera;

        available.Remove(CurrentCamera);
        if (available.Count == 0) return CurrentCamera;

        var recent = _lastCameras.Count >= 2
            ? _lastCameras.Skip(_lastCameras.Count - 2).ToList()
            : new List<long>();

        var notRecent = available.Where(c => !recent.Contains(c)).ToList();
        var pool = notRecent.Count > 0 ? notRecent : available;
        return pool[_rng.Next(pool.Count)];
    }

    public void SwitchCamera(long cam)
    {
        if (TransitionMode == "mix")
        {
            int frames = Math.Max(1, (int)Math.Round(FadeDurationSeconds * 30));
            _driver.FadeTo(cam, frames);
        }
        else
        {
            _driver.CutTo(cam);
        }

        CurrentCamera = cam;
        SwitchCount++;
        _lastCameras.Add(cam);
        if (_lastCameras.Count > 3) _lastCameras.RemoveAt(0);
    }

    // El "latido" del loop automatico (identico a auto_switcher_loop de Python).
    public void Tick()
    {
        if (!AutoMode) return;

        // Modo secuencia manual: reproduce los pasos en orden, en bucle.
        if (SequenceMode)
        {
            if (Sequence.Count == 0) return;
            if (DateTime.Now >= NextSwitchTime)
            {
                _seqIndex = (_seqIndex + 1) % Sequence.Count;
                var step = Sequence[_seqIndex];
                SwitchStep(step);
                NextSwitchTime = DateTime.Now.AddSeconds(Math.Max(1, step.Seconds));
            }
            return;
        }

        // Modo aleatorio (anti-repeticion).
        if (AvailableCameras.Count == 0) return;
        if (DateTime.Now >= NextSwitchTime)
        {
            SwitchCamera(GetNextCamera());
            NextSwitchTime = DateTime.Now.AddSeconds(IntervalSeconds);
        }
    }

    private void SwitchStep(SeqStep step)
    {
        if (step.Transition == "mix")
            _driver.FadeTo(step.Camera, Math.Max(1, (int)Math.Round(step.Fade * 30)));
        else
            _driver.CutTo(step.Camera);
        CurrentCamera = step.Camera;
        SwitchCount++;
    }

    // Arranca la secuencia desde el primer paso.
    public void StartSequence()
    {
        _seqIndex = -1;
        NextSwitchTime = DateTime.Now;
        AutoMode = true;
    }

    public void ForceCamera(long cam)
    {
        AutoMode = false;      // forzar pausa el automatico
        SwitchCamera(cam);
    }

    public void Pause() => AutoMode = false;

    public void Resume()
    {
        AutoMode = true;
        NextSwitchTime = DateTime.Now.AddSeconds(IntervalSeconds);
    }

    public int SecondsUntilNext =>
        Math.Max(0, (int)Math.Ceiling((NextSwitchTime - DateTime.Now).TotalSeconds));
}

namespace AutoSwitcher.Core;

// Info de una camara/entrada, agnostica de marca.
public record CameraInfo(long Id, string Name);

// Contrato que cualquier "chofer" (ATEM, vMix, OBS, Roland...) debe cumplir.
// El cerebro solo conoce esta interfaz, nunca una marca concreta.
public interface ISwitcherDriver
{
    string ProductName { get; }
    IReadOnlyList<CameraInfo> Cameras { get; }
    long ProgramInput { get; }

    // Corte directo a una camara.
    void CutTo(long cameraId);

    // Fundido (mezcla) a una camara, en 'frames' cuadros.
    void FadeTo(long cameraId, int frames);
}

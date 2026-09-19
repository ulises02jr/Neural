using System.Runtime.InteropServices;
using BMDSwitcherAPI;
using AutoSwitcher.Core;

namespace AutoSwitcher.Atem;

// FASE 2 - El "chofer" del ATEM. Habla con el SDK oficial de Blackmagic.
// Solo esta clase conoce el ATEM; el cerebro no.
public sealed class AtemDriver : ISwitcherDriver
{
    private IBMDSwitcher _switcher = null!;
    private IBMDSwitcherMixEffectBlock _me = null!;
    private IBMDSwitcherTransitionParameters _transition = null!;
    private IBMDSwitcherTransitionMixParameters _mix = null!;
    private readonly List<CameraInfo> _cameras = new();

    public string ProductName { get; private set; } = "";
    public IReadOnlyList<CameraInfo> Cameras => _cameras;

    public long ProgramInput
    {
        get { _me.GetProgramInput(out long id); return id; }
    }

    public void Connect(string ip)
    {
        var discovery = new CBMDSwitcherDiscovery();
        discovery.ConnectTo(ip, out _switcher, out _BMDSwitcherConnectToFailure failure);
        _switcher.GetProductName(out string name);
        ProductName = name;

        _me = GetMixEffectBlocks(_switcher).First();
        _transition = (IBMDSwitcherTransitionParameters)_me;
        _mix = (IBMDSwitcherTransitionMixParameters)_me;

        DetectCameras();
    }

    // "Chofer inteligente": lee del ATEM cuales entradas son camaras externas.
    private void DetectCameras()
    {
        _cameras.Clear();
        foreach (var input in GetInputs(_switcher))
        {
            input.GetPortType(out _BMDSwitcherPortType type);
            if (type != _BMDSwitcherPortType.bmdSwitcherPortTypeExternal) continue;
            input.GetInputId(out long id);
            string name = "";
            try { input.GetLongName(out name); } catch { }
            _cameras.Add(new CameraInfo(id, name));
        }
    }

    public void CutTo(long cameraId)
    {
        _me.SetProgramInput(cameraId);
    }

    public void FadeTo(long cameraId, int frames)
    {
        _transition.SetNextTransitionSelection(
            _BMDSwitcherTransitionSelection.bmdSwitcherTransitionSelectionBackground);
        _transition.SetNextTransitionStyle(
            _BMDSwitcherTransitionStyle.bmdSwitcherTransitionStyleMix);
        _mix.SetRate((uint)Math.Max(1, frames));
        _me.SetPreviewInput(cameraId);
        _me.PerformAutoTransition();
    }

    // --- Iteradores COM del SDK ---
    private static IEnumerable<IBMDSwitcherInput> GetInputs(IBMDSwitcher sw)
    {
        sw.CreateIterator(typeof(IBMDSwitcherInputIterator).GUID, out IntPtr ptr);
        var it = Marshal.GetObjectForIUnknown(ptr) as IBMDSwitcherInputIterator;
        if (it == null) yield break;
        while (true)
        {
            it.Next(out IBMDSwitcherInput input);
            if (input == null) yield break;
            yield return input;
        }
    }

    private static IEnumerable<IBMDSwitcherMixEffectBlock> GetMixEffectBlocks(IBMDSwitcher sw)
    {
        sw.CreateIterator(typeof(IBMDSwitcherMixEffectBlockIterator).GUID, out IntPtr ptr);
        var it = Marshal.GetObjectForIUnknown(ptr) as IBMDSwitcherMixEffectBlockIterator;
        if (it == null) yield break;
        while (true)
        {
            it.Next(out IBMDSwitcherMixEffectBlock me);
            if (me == null) yield break;
            yield return me;
        }
    }
}

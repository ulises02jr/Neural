using System.Runtime.InteropServices;
using BMDSwitcherAPI;

namespace AutoSwitcher.AtemProbe;

// FASE 1 - "Prueba de vida": conectarse al ATEM por IP,
// listar las cámaras que reporta, y cortar a una cámara.
internal static class Program
{
    static int Main(string[] args)
    {
        string ip = args.Length > 0 ? args[0] : "172.28.135.97";
        int camera = args.Length > 1 && int.TryParse(args[1], out var c) ? c : 2;

        Console.WriteLine("==================================================");
        Console.WriteLine("  AutoSwitcher - Prueba de vida (Fase 1)");
        Console.WriteLine("==================================================");
        Console.WriteLine($"ATEM IP : {ip}");
        Console.WriteLine($"Camara  : {camera}");
        Console.WriteLine();

        IBMDSwitcher switcher;
        try
        {
            var discovery = new CBMDSwitcherDiscovery();
            Console.WriteLine("Conectando al ATEM...");
            discovery.ConnectTo(ip, out switcher, out _BMDSwitcherConnectToFailure failure);
            Console.WriteLine("[OK] Conectado al ATEM.");
        }
        catch (Exception ex)
        {
            Console.WriteLine("[ERROR] No se pudo conectar.");
            Console.WriteLine("   " + ex.Message);
            Console.WriteLine("   Revisa: IP correcta, cable de red, ATEM encendido,");
            Console.WriteLine("   y que ATEM Software Control este instalado.");
            Console.WriteLine("Presiona ENTER para salir...");
            Console.ReadLine();
            return 1;
        }

        try { switcher.GetProductName(out string product); Console.WriteLine($"Modelo detectado: {product}"); }
        catch { }

        Console.WriteLine();
        Console.WriteLine("Entradas externas (camaras) detectadas:");
        foreach (var input in GetInputs(switcher))
        {
            input.GetPortType(out _BMDSwitcherPortType type);
            if (type != _BMDSwitcherPortType.bmdSwitcherPortTypeExternal) continue;
            input.GetInputId(out long id);
            string name = "";
            try { input.GetLongName(out name); } catch { }
            Console.WriteLine($"   - Entrada {id}: {name}");
        }

        var me0 = GetMixEffectBlocks(switcher).FirstOrDefault();
        if (me0 == null)
        {
            Console.WriteLine("[ERROR] No se encontro un bloque M/E.");
            Console.ReadLine();
            return 1;
        }

        Console.WriteLine();
        Console.WriteLine($"Cortando a la camara {camera}...");
        me0.SetProgramInput(camera);
        Console.WriteLine($"[OK] Programa ahora en la camara {camera}.");

        Console.WriteLine();
        Console.WriteLine("Prueba de vida completa. Presiona ENTER para salir...");
        Console.ReadLine();
        return 0;
    }

    static IEnumerable<IBMDSwitcherInput> GetInputs(IBMDSwitcher switcher)
    {
        switcher.CreateIterator(typeof(IBMDSwitcherInputIterator).GUID, out IntPtr ptr);
        var it = Marshal.GetObjectForIUnknown(ptr) as IBMDSwitcherInputIterator;
        if (it == null) yield break;
        while (true)
        {
            it.Next(out IBMDSwitcherInput input);
            if (input == null) yield break;
            yield return input;
        }
    }

    static IEnumerable<IBMDSwitcherMixEffectBlock> GetMixEffectBlocks(IBMDSwitcher switcher)
    {
        switcher.CreateIterator(typeof(IBMDSwitcherMixEffectBlockIterator).GUID, out IntPtr ptr);
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

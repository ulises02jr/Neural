using AutoSwitcher.Core;
using AutoSwitcher.Atem;

string ip = args.Length > 0 ? args[0] : "172.28.135.97";

Console.WriteLine("==================================================");
Console.WriteLine("  AutoSwitcher - Cerebro + Chofer (Fase 2 y 3)");
Console.WriteLine("==================================================");
Console.WriteLine($"Conectando al ATEM en {ip}...");

var driver = new AtemDriver();
try
{
    driver.Connect(ip);
}
catch (Exception ex)
{
    Console.WriteLine("[ERROR] No se pudo conectar: " + ex.Message);
    Console.WriteLine("Revisa IP, red, ATEM encendido y ATEM Software Control.");
    Console.WriteLine("Presiona ENTER para salir...");
    Console.ReadLine();
    return;
}

Console.WriteLine($"[OK] Conectado: {driver.ProductName}");
Console.WriteLine("Camaras detectadas: " +
    string.Join(", ", driver.Cameras.Select(c => $"{c.Id}={c.Name}")));

var brain = new SwitcherBrain(driver);

Console.WriteLine();
Console.WriteLine("CONTROLES:");
Console.WriteLine("  [P] Pausar   [R] Reanudar   [1-9] Forzar camara");
Console.WriteLine("  [C] Corte    [M] Mezcla     [+/-] Intervalo   [Q] Salir");
Console.WriteLine();

bool running = true;
while (running)
{
    if (Console.KeyAvailable)
    {
        var k = Console.ReadKey(true).KeyChar;
        switch (char.ToLowerInvariant(k))
        {
            case 'p': brain.Pause(); break;
            case 'r': brain.Resume(); break;
            case 'c': brain.TransitionMode = "cut"; break;
            case 'm': brain.TransitionMode = "mix"; break;
            case '+': brain.IntervalSeconds = Math.Min(60, brain.IntervalSeconds + 1); break;
            case '-': brain.IntervalSeconds = Math.Max(3, brain.IntervalSeconds - 1); break;
            case 'q': running = false; break;
            default:
                if (char.IsDigit(k))
                {
                    long cam = k - '0';
                    if (brain.AvailableCameras.Contains(cam)) brain.ForceCamera(cam);
                }
                break;
        }
    }

    brain.Tick();
    PrintStatus(brain);
    Thread.Sleep(200);
}

Console.WriteLine("\n\nDetenido. Cambios: " + brain.SwitchCount);

static void PrintStatus(SwitcherBrain b)
{
    string modo = b.AutoMode ? "AUTO " : "MANUAL";
    string trans = b.TransitionMode == "mix" ? "Mezcla" : "Corte ";
    string prox = b.AutoMode ? $"{b.SecondsUntilNext,2}s" : "--";
    string linea =
        $"\r[{modo}] CAM {b.CurrentCamera}  |  Trans: {trans}  |  " +
        $"Int: {b.IntervalSeconds,2}s  |  Prox: {prox}  |  Cambios: {b.SwitchCount}   ";
    Console.Write(linea);
}

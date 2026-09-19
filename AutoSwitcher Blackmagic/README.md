# AutoSwitcher

Switcher automatico de camaras para ATEM (Blackmagic), en C# sobre el SDK oficial.
Portado desde el prototipo original en Python (switcher_completo.py).

## Arquitectura (cerebro + chofer)

- `ISwitcherDriver.cs` - contrato agnostico de marca (permite vMix/OBS/Roland a futuro).
- `AtemDriver.cs`      - chofer del ATEM (SDK oficial de Blackmagic).
- `SwitcherBrain.cs`   - cerebro: algoritmo anti-repeticion, intervalos, corte/fade, loop.
- `Program.cs`         - arranque + control por teclado.

## Requisitos en la maquina (Windows)

- .NET 8 SDK
- ATEM Software Control instalado (registra el componente del ATEM)
- El ATEM en la misma red

## Compilar

    dotnet build src\AutoSwitcher.Cli\AutoSwitcher.Cli.csproj -c Release

## Correr

    cd src\AutoSwitcher.Cli\bin\Release\net8.0-windows
    .\AutoSwitcher.exe <IP-DEL-ATEM>

Controles: [P] pausar  [R] reanudar  [1-9] forzar camara
           [C] corte   [M] mezcla    [+/-] intervalo  [Q] salir

## Nota tecnica

El "puente" al COM del ATEM (lib/Interop.BMDSwitcherAPI.dll) se pre-genera con
tools/gen-interop.ps1 usando herramientas propias de Windows. Asi el proyecto
compila con 'dotnet build' sin necesitar Visual Studio completo.

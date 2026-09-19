$ErrorActionPreference = 'Stop'
$outDir = 'C:\Users\miigl\Dev\AutoSwitcher\lib'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$src = @'
using System;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;

public class TlbGen : ITypeLibImporterNotifySink
{
    [DllImport("oleaut32.dll", CharSet=CharSet.Unicode, PreserveSig=false)]
    static extern void LoadRegTypeLib(ref Guid rguid, ushort maj, ushort min, int lcid,
        [MarshalAs(UnmanagedType.Interface)] out object ppTLB);

    public void ReportEvent(ImporterEventKind k, int c, string m) { }

    public Assembly ResolveRef(object tlb)
    {
        var conv = new TypeLibConverter();
        return conv.ConvertTypeLibToAssembly(tlb, "ref_" + Guid.NewGuid().ToString("N") + ".dll",
            TypeLibImporterFlags.None, this, null, null, null, null);
    }

    public static void Run(string guid, ushort maj, ushort min, string fileName, string ns)
    {
        Guid g = new Guid(guid);
        object tlb;
        LoadRegTypeLib(ref g, maj, min, 0, out tlb);
        var conv = new TypeLibConverter();
        AssemblyBuilder ab = conv.ConvertTypeLibToAssembly(tlb, fileName,
            TypeLibImporterFlags.None, new TlbGen(), null, null, ns, null);
        ab.Save(fileName);
    }
}
'@

Add-Type -TypeDefinition $src -Language CSharp
[System.IO.Directory]::SetCurrentDirectory($outDir)
[TlbGen]::Run('{8A92B919-156C-4D61-94EF-03F9BE4004B0}', 1, 0, 'Interop.BMDSwitcherAPI.dll', 'BMDSwitcherAPI')
Write-Output ("EXISTE: " + (Test-Path (Join-Path $outDir 'Interop.BMDSwitcherAPI.dll')))
Get-ChildItem $outDir -Filter *.dll | Select-Object Name,Length | Format-Table -AutoSize | Out-String

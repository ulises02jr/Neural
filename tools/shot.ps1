$sig = @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
'@
Add-Type -MemberDefinition $sig -Name W -Namespace NX
$p = Get-Process AutoSwitcher -EA SilentlyContinue | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object -First 1
if($p){ [NX.W]::ShowWindow($p.MainWindowHandle,9); [NX.W]::SetForegroundWindow($p.MainWindowHandle) }
Start-Sleep 2
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$full=New-Object System.Drawing.Bitmap $b.Width,$b.Height
$g=[System.Drawing.Graphics]::FromImage($full)
$g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size)
$w=1100;$h=[int]($b.Height*($w/$b.Width))
$small=New-Object System.Drawing.Bitmap $w,$h
$g2=[System.Drawing.Graphics]::FromImage($small)
$g2.DrawImage($full,0,0,$w,$h)
$small.Save('C:\Users\miigl\Dev\AutoSwitcher\screen7.jpg',[System.Drawing.Imaging.ImageFormat]::Jpeg)
Write-Output done

# Launch the published EXE with --stay --settings, capture the screen, then stop.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$exe = "C:\Users\Mayn\Desktop\hotspot\PowerToysRunStandalone\publish\PowerToysRunStandalone.exe"
$out = "C:\Users\Mayn\Desktop\hotspot\PowerToysRunStandalone\artifacts\settings-window.png"

if (!(Test-Path $exe)) { throw "EXE not found: $exe" }
if (!(Test-Path (Split-Path $out))) { New-Item -ItemType Directory -Path (Split-Path $out) | Out-Null }

$p = Start-Process -FilePath $exe -ArgumentList '--stay','--settings' -PassThru
Start-Sleep -Seconds 6

$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bmp.Size)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

Write-Output "CAPTURED $out ($($bounds.Width) x $($bounds.Height))"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
exit 0

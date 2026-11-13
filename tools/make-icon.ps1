# Generates res\app.ico (multi-size, PNG-compressed entries). Only needed if you want to change the icon.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\res\app.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function New-IconPng([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    # rounded dark tile
    $r = [Math]::Max(2, $s * 0.2)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w = $s - 1
    $path.AddArc(0, 0, $r * 2, $r * 2, 180, 90)
    $path.AddArc($w - $r * 2, 0, $r * 2, $r * 2, 270, 90)
    $path.AddArc($w - $r * 2, $w - $r * 2, $r * 2, $r * 2, 0, 90)
    $path.AddArc(0, $w - $r * 2, $r * 2, $r * 2, 90, 90)
    $path.CloseFigure()
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 0)), ([System.Drawing.PointF]::new(0, $s)), ([System.Drawing.Color]::FromArgb(255, 38, 50, 72)), ([System.Drawing.Color]::FromArgb(255, 20, 26, 38))
    $g.FillPath($bg, $path)

    # graph line + fill
    $pts = @(@(0.12, 0.72), @(0.30, 0.52), @(0.44, 0.62), @(0.62, 0.28), @(0.76, 0.42), @(0.88, 0.22)) |
        ForEach-Object { [System.Drawing.PointF]::new($_[0] * $s, $_[1] * $s) }
    $fill = New-Object System.Collections.Generic.List[System.Drawing.PointF]
    $pts | ForEach-Object { $fill.Add($_) }
    $fill.Add([System.Drawing.PointF]::new(0.88 * $s, 0.86 * $s))
    $fill.Add([System.Drawing.PointF]::new(0.12 * $s, 0.86 * $s))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(110, 79, 195, 247))), $fill.ToArray())
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 79, 195, 247)), ([Math]::Max(1.2, $s / 14))
    $pen.LineJoin = 'Round'
    $g.DrawLines($pen, $pts)
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

$images = foreach ($s in $sizes) { , (New-IconPng $s) }
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = $images[$i]
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$d.Length); $bw.Write([UInt32]$offset)
    $offset += $d.Length
}
foreach ($d in $images) { $bw.Write($d) }
$bw.Close()
Write-Host "Wrote $out"

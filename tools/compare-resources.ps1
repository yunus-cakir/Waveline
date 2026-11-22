# Waveline Clip vs SteelSeries Moments: side-by-side resource usage.
# Samples CPU, RAM, GPU 3D, NVENC and GPU memory per program group every few seconds and
# prints averages/peaks at the end. Run while both are recording (e.g. during a game).
#
#   .\tools\compare-resources.ps1 -Minutes 5
param(
    [double]$Minutes = 5,
    [int]$IntervalSeconds = 5
)
$ErrorActionPreference = 'SilentlyContinue'

# Sonar is left out on purpose: both recorders need it.
$groups = [ordered]@{
    'Waveline Clip'       = @('WavelineClip')
    'Moments'             = @('SteelSeriesMoments', 'SteelSeriesCaptureSvc')
    'GG altyapisi'        = @('SteelSeriesGG', 'SteelSeriesGGClient', 'SteelSeriesEngine', 'SteelSeriesGGEZ', 'SteelSeriesPrism')
}
$cores = [Environment]::ProcessorCount

function Get-GpuByPid {
    $byPid = @{}
    $engines = (Get-Counter '\GPU Engine(*)\Utilization Percentage').CounterSamples
    foreach ($s in $engines) {
        if ($s.InstanceName -match 'pid_(\d+)_.*engtype_(\w+)') {
            $procId = [int]$Matches[1]; $type = $Matches[2]
            if (-not $byPid[$procId]) { $byPid[$procId] = @{ '3D' = 0.0; 'VideoEncode' = 0.0; 'Mem' = 0.0 } }
            if ($type -eq '3D' -or $type -eq 'VideoEncode') { $byPid[$procId][$type] += $s.CookedValue }
        }
    }
    $mem = (Get-Counter '\GPU Process Memory(*)\Dedicated Usage').CounterSamples
    foreach ($s in $mem) {
        if ($s.InstanceName -match 'pid_(\d+)_') {
            $procId = [int]$Matches[1]
            if (-not $byPid[$procId]) { $byPid[$procId] = @{ '3D' = 0.0; 'VideoEncode' = 0.0; 'Mem' = 0.0 } }
            $byPid[$procId]['Mem'] += $s.CookedValue / 1MB
        }
    }
    $byPid
}

$stats = @{}
foreach ($g in $groups.Keys) { $stats[$g] = @{ n = 0; cpu = 0.0; ram = 0.0; d3 = 0.0; enc = 0.0; vram = 0.0; pcpu = 0.0; pram = 0.0; penc = 0.0 } }

$lastCpu = @{}
$lastTime = Get-Date
foreach ($p in Get-Process) { $lastCpu[$p.Id] = $p.TotalProcessorTime.TotalSeconds }
$end = (Get-Date).AddMinutes($Minutes)
Write-Host "Olculuyor: $Minutes dk, $IntervalSeconds sn arayla. Kayit yapan oyunu acik tutun..."

while ((Get-Date) -lt $end) {
    Start-Sleep -Seconds $IntervalSeconds
    $now = Get-Date
    $elapsed = ($now - $lastTime).TotalSeconds
    $lastTime = $now
    $gpu = Get-GpuByPid
    $line = @()
    foreach ($g in $groups.Keys) {
        $procs = Get-Process -Name $groups[$g]
        $cpu = 0.0; $ram = 0.0; $d3 = 0.0; $enc = 0.0; $vram = 0.0
        foreach ($p in $procs) {
            $t = $p.TotalProcessorTime.TotalSeconds
            if ($lastCpu.ContainsKey($p.Id)) { $cpu += ($t - $lastCpu[$p.Id]) / $elapsed / $cores * 100 }
            $lastCpu[$p.Id] = $t
            $ram += $p.WorkingSet64 / 1MB
            if ($gpu[$p.Id]) { $d3 += $gpu[$p.Id]['3D']; $enc += $gpu[$p.Id]['VideoEncode']; $vram += $gpu[$p.Id]['Mem'] }
        }
        if (-not $procs) { $line += "{0}: calismiyor" -f $g; continue }
        $s = $stats[$g]
        $s.n++; $s.cpu += $cpu; $s.ram += $ram; $s.d3 += $d3; $s.enc += $enc; $s.vram += $vram
        $s.pcpu = [Math]::Max($s.pcpu, $cpu); $s.pram = [Math]::Max($s.pram, $ram); $s.penc = [Math]::Max($s.penc, $enc)
        $line += "{0}: CPU %{1:N1} RAM {2:N0}MB NVENC %{3:N0}" -f $g, $cpu, $ram, $enc
    }
    Write-Host ("[{0:HH:mm:ss}] " -f $now) ($line -join ' | ')
}

Write-Host ""
Write-Host "=== Ortalama (tepe) ==="
'{0,-15} {1,14} {2,16} {3,10} {4,14} {5,10}' -f 'Grup', 'CPU %', 'RAM MB', 'GPU 3D %', 'NVENC %', 'VRAM MB' | Write-Host
foreach ($g in $groups.Keys) {
    $s = $stats[$g]
    if ($s.n -eq 0) { '{0,-15} calismiyor' -f $g | Write-Host; continue }
    '{0,-15} {1,14} {2,16} {3,10:N1} {4,14} {5,10:N0}' -f $g,
        ('{0:N2} ({1:N1})' -f ($s.cpu / $s.n), $s.pcpu),
        ('{0:N0} ({1:N0})' -f ($s.ram / $s.n), $s.pram),
        ($s.d3 / $s.n),
        ('{0:N1} ({1:N0})' -f ($s.enc / $s.n), $s.penc),
        ($s.vram / $s.n) | Write-Host
}
Write-Host ""
Write-Host "Not: Moments, GG altyapisi olmadan calismaz; Waveline Clip icin GG gerekmez (Sonar haric)."

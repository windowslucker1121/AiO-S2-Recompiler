# SPDX-License-Identifier: GPL-3.0-or-later
param([string]$Out, [int]$Seconds = 200)
# Every 5 s: processes (other than the game) that used more than 0.3 cores in that interval, and the GPU's clock, load, power and memory.
$prev = @{}
Get-Process | % { $prev[$_.Id] = $_.CPU }
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep 5
    $t = [int]((Get-Date) - $t0).TotalSeconds
    $line = @()
    Get-Process | % {
        $c = $_.CPU; if ($c -eq $null) { return }
        $p = $prev[$_.Id]; $prev[$_.Id] = $c
        if ($p -ne $null -and $_.Name -ne 'oot' -and ($c - $p) / 5 -gt 0.3) { $line += ('{0}:{1:N1}' -f $_.Name, (($c - $p) / 5)) }
    }
    $gpu = (nvidia-smi --query-gpu=clocks.gr,utilization.gpu,power.draw,memory.used,temperature.gpu --format=csv,noheader) -join ' '
    Add-Content $Out ("{0} t+{1}s gpu[{2}] {3}" -f (Get-Date -Format HH:mm:ss), $t, $gpu, ($line -join ' '))
}

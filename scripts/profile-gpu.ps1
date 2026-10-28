param(
  [Parameter(Mandatory=$true)][string]$Exe,
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [string]$RestoreExe,
  [int]$Samples=6,
  [string[]]$Scenes=@('pet-frozen','pet','static','running','running-no-shimmer','running-no-spinner','running-no-acrylic','running-frozen-gif','running-no-shimmer-no-spinner','running')
)
$ErrorActionPreference='Stop'
$Exe=(Resolve-Path -LiteralPath $Exe).Path
if (!$RestoreExe) { $RestoreExe=$Exe }
$RestoreExe=(Resolve-Path -LiteralPath $RestoreExe).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$logicalProcessors=[Environment]::ProcessorCount
$dwmPid=(Get-Process -Name dwm | Select-Object -First 1).Id
$sunshinePid=(Get-Process -Name sunshine -ErrorAction SilentlyContinue | Select-Object -First 1).Id
$results=[Collections.Generic.List[object]]::new()
$raw=[Collections.Generic.List[object]]::new()

function Stop-ProfilePet {
  $running=Get-Process -Name ZCodePet -ErrorAction SilentlyContinue
  Start-Process -FilePath $Exe -ArgumentList '--quit' -Wait -WindowStyle Hidden
  foreach ($item in $running) {
    if (!$item.WaitForExit(8000)) { throw "Pet process $($item.Id) did not exit." }
  }
}
function Read-Probe {
  Start-Process -FilePath $Exe -ArgumentList '--probe' -Wait -WindowStyle Hidden
  $values=@{}
  $source=Get-Content -LiteralPath (Join-Path (Split-Path $Exe) 'probe.txt') -Raw
  foreach ($match in [regex]::Matches($source,'(\w+)=([0-9.]+)')) {
    $values[$match.Groups[1].Value]=[double]$match.Groups[2].Value
  }
  return $values
}
function Read-Gpu([int]$appPid) {
  $values=@{}
  foreach ($row in Get-CimInstance Win32_PerfRawData_GPUPerformanceCounters_GPUEngine) {
    if ($row.Name -match '^pid_(\d+)_.*_engtype_(.+)$') {
      $samplePid=[int]$Matches[1]
      $engine=$Matches[2]
      if ($samplePid -in @($appPid,$dwmPid,$sunshinePid)) {
        $values[$row.Name]=[pscustomobject]@{
          ProcessId=$samplePid; Engine=$engine;
          Running=[uint64]$row.UtilizationPercentage;
          Timestamp=[uint64]$row.Timestamp_Sys100NS
        }
      }
    }
  }
  return $values
}
function Mean($values) {
  if (!$values -or $values.Count -eq 0) { return 0 }
  return [double]($values | Measure-Object -Average).Average
}

try {
  for ($sceneIndex=0;$sceneIndex -lt $Scenes.Count;$sceneIndex++) {
    $scene=$Scenes[$sceneIndex]
    Stop-ProfilePet
    $process=Start-Process -FilePath $Exe -ArgumentList "--gpu-profile=$scene" -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2
    $before=Read-Probe
    $previous=Read-Gpu $process.Id
    $cpuBefore=(Get-Process -Id $process.Id).TotalProcessorTime.TotalSeconds
    $watch=[Diagnostics.Stopwatch]::StartNew()
    $summary=[Collections.Generic.List[object]]::new()
    for ($sampleIndex=0;$sampleIndex -lt $Samples;$sampleIndex++) {
      Start-Sleep -Seconds 1
      $current=Read-Gpu $process.Id
      $interval=[Collections.Generic.List[object]]::new()
      foreach ($name in $current.Keys) {
        if (!$previous.ContainsKey($name)) { continue }
        $a=$previous[$name]; $b=$current[$name]
        if ($b.Timestamp -le $a.Timestamp -or $b.Running -lt $a.Running) { continue }
        $percent=100.0*($b.Running-$a.Running)/($b.Timestamp-$a.Timestamp)
        $entry=[pscustomobject]@{ SceneIndex=$sceneIndex; Scene=$scene; Sample=$sampleIndex; ProcessId=$b.ProcessId; Engine=$b.Engine; EngineInstance=$name; Percent=$percent }
        $interval.Add($entry); $raw.Add($entry)
      }
      $app3d=@($interval | Where-Object { $_.ProcessId -eq $process.Id -and $_.Engine -eq '3D' } | Sort-Object Percent -Descending)
      $dwm3d=@($interval | Where-Object { $_.ProcessId -eq $dwmPid -and $_.Engine -eq '3D' } | Sort-Object Percent -Descending)
      $encode=@($interval | Where-Object { $_.ProcessId -eq $sunshinePid -and $_.Engine -match 'VideoEncode' } | Sort-Object Percent -Descending)
      $summary.Add([pscustomobject]@{ App3d=if($app3d.Count){$app3d[0].Percent}else{0}; Dwm3d=if($dwm3d.Count){$dwm3d[0].Percent}else{0}; Encode=if($encode.Count){$encode[0].Percent}else{0} })
      $previous=$current
    }
    $elapsed=$watch.Elapsed.TotalSeconds
    $cpuAfter=(Get-Process -Id $process.Id).TotalProcessorTime.TotalSeconds
    $after=Read-Probe
    $renderElapsed=($after.renderTimeMicros-$before.renderTimeMicros)/1000000.0
    $memory=@(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory | Where-Object { $_.Name -like "pid_$($process.Id)_*" })
    $hardware=@(& nvidia-smi --query-gpu=utilization.gpu,power.draw,clocks.gr --format=csv,noheader,nounits)[0].Split(',')
    $result=[pscustomobject]@{
      SceneIndex=$sceneIndex; Scene=$scene; Seconds=$elapsed;
      App3dPercent=(Mean @($summary.App3d)); App3dMin=($summary.App3d | Measure-Object -Minimum).Minimum; App3dMax=($summary.App3d | Measure-Object -Maximum).Maximum;
      Dwm3dPercent=(Mean @($summary.Dwm3d)); SunshineVideoEncodePercent=(Mean @($summary.Encode));
      CpuPercent=100*($cpuAfter-$cpuBefore)/$elapsed/$logicalProcessors;
      PanelPaintsPerSecond=($after.panelDraws-$before.panelDraws)/$renderElapsed;
      PetUploadsPerSecond=($after.petDraws-$before.petDraws)/$renderElapsed;
      VisualFramesPerSecond=($after.visualFrames-$before.visualFrames)/$renderElapsed;
      DedicatedMiB=($memory | Measure-Object DedicatedUsage -Sum).Sum/1MB;
      SharedMiB=($memory | Measure-Object SharedUsage -Sum).Sum/1MB;
      BoardUtilizationPercent=[double]$hardware[0]; BoardPowerWatts=[double]$hardware[1]; BoardGraphicsClockMHz=[double]$hardware[2];
      DisplayHz=$after.displayHz; VblankClock=$after.vblankClock
    }
    $results.Add($result)
    $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
    $results | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'summary.csv') -NoTypeInformation -Encoding utf8
    $raw | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'engine-samples.csv') -NoTypeInformation -Encoding utf8
    Write-Output ("{0}: app3D={1:N3}% dwm3D={2:N3}% panel={3:N1}/s pet={4:N1}/s cpu={5:N3}%" -f $scene,$result.App3dPercent,$result.Dwm3dPercent,$result.PanelPaintsPerSecond,$result.PetUploadsPerSecond,$result.CpuPercent)
  }
} finally {
  Stop-ProfilePet
  Start-Process -FilePath $RestoreExe -ArgumentList '--panel' -WindowStyle Hidden
}

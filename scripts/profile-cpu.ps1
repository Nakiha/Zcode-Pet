param(
  [Parameter(Mandatory=$true)][string]$BeforeExe,
  [Parameter(Mandatory=$true)][string]$AfterExe,
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [string]$RestoreExe,
  [int]$Samples=120,
  [int]$IntervalMs=100
)
$ErrorActionPreference='Stop'
$BeforeExe=(Resolve-Path -LiteralPath $BeforeExe).Path
$AfterExe=(Resolve-Path -LiteralPath $AfterExe).Path
if(!$RestoreExe){$RestoreExe=$AfterExe}
$RestoreExe=(Resolve-Path -LiteralPath $RestoreExe).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$taskRows=[Collections.Generic.List[object]]::new()
$taskSummary=[Collections.Generic.List[object]]::new()
$taskCores=[Environment]::ProcessorCount
function Stop-TaskPet {
  $taskOld=@(Get-Process -Name ZCodePet -ErrorAction SilentlyContinue)
  Start-Process -FilePath $BeforeExe -ArgumentList '--quit' -Wait -WindowStyle Hidden
  foreach($taskProcess in $taskOld) {
    if(!$taskProcess.WaitForExit(8000)){throw 'Pet did not exit'}
  }
}
try {
  foreach($taskMode in @('before','after')) {
    Stop-TaskPet
    $taskExe=if($taskMode -eq 'before'){$BeforeExe}else{$AfterExe}
    $taskProcess=Start-Process -FilePath $taskExe -ArgumentList '--panel' -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 2500
    $taskProcess.Refresh()
    $taskPrevious=$taskProcess.TotalProcessorTime.TotalMilliseconds
    $taskCpuStart=$taskPrevious
    $taskClock=[Diagnostics.Stopwatch]::StartNew()
    $taskLast=0.0
    $taskValues=[Collections.Generic.List[double]]::new()
    for($taskSample=0;$taskSample -lt $Samples;++$taskSample) {
      Start-Sleep -Milliseconds $IntervalMs
      $taskProcess.Refresh()
      $taskCpu=$taskProcess.TotalProcessorTime.TotalMilliseconds
      $taskNow=$taskClock.Elapsed.TotalMilliseconds
      $taskPercent=100*($taskCpu-$taskPrevious)/($taskNow-$taskLast)/$taskCores
      $taskValues.Add($taskPercent)
      $taskRows.Add([pscustomobject]@{Build=$taskMode;Sample=$taskSample;CpuPercent=$taskPercent;CpuMs=$taskCpu-$taskPrevious;ElapsedMs=$taskNow-$taskLast})
      $taskPrevious=$taskCpu;$taskLast=$taskNow
    }
    $taskSorted=@($taskValues | Sort-Object)
    $taskSummary.Add([pscustomobject]@{Build=$taskMode;Samples=$Samples;LogicalProcessors=$taskCores;AveragePercent=100*($taskPrevious-$taskCpuStart)/$taskLast/$taskCores;PeakPercent=$taskSorted[-1];P95Percent=$taskSorted[[int][math]::Floor(($Samples-1)*.95)]})
  }
} finally {
  Stop-TaskPet
  Start-Process -FilePath $RestoreExe -ArgumentList '--panel' -WindowStyle Hidden
  $taskRows | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'cpu-samples.csv') -NoTypeInformation -Encoding utf8
  $taskSummary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
}
$taskSummary

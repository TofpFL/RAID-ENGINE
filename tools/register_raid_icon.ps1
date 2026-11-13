# Registers the .RAID extension with RAID_File_Icon.ico (per-user, no admin).
# Usage: powershell -File tools/register_raid_icon.ps1 [-IconPath <path-to-ico>]
param(
    [string]$IconPath = (Join-Path $PSScriptRoot "..\RAID_File_Icon.ico")
)
$IconPath = (Resolve-Path $IconPath).Path
$progId = "RAID.Engine.Chart"

New-Item -Path "HKCU:\Software\Classes\.RAID" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\.RAID" -Name "(Default)" -Value $progId
New-Item -Path "HKCU:\Software\Classes\$progId" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\$progId" -Name "(Default)" -Value "RAID Chart"
New-Item -Path "HKCU:\Software\Classes\$progId\DefaultIcon" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\$progId\DefaultIcon" -Name "(Default)" -Value "`"$IconPath`""

$sig = '[System.Runtime.InteropServices.DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);'
$shc = Add-Type -MemberDefinition $sig -Name SHC$PID -Namespace W$PID -PassThru
$shc::SHChangeNotify(0x08000000, 0, [System.IntPtr]::Zero, [System.IntPtr]::Zero)
Write-Output ".RAID -> $progId ($IconPath)"

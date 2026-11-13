# System-wide (HKLM) .RAID icon registration — run elevated:
#   powershell -File tools/register_raid_icon_machine.ps1 [-IconPath <ico>]
param(
    [string]$IconPath = (Join-Path $PSScriptRoot "..\RAID_File_Icon.ico")
)
$IconPath = (Resolve-Path $IconPath).Path
$progId = "RAID.Engine.Chart"

New-Item -Path "HKLM:\SOFTWARE\Classes\.RAID" -Force | Out-Null
Set-ItemProperty -Path "HKLM:\SOFTWARE\Classes\.RAID" -Name "(Default)" -Value $progId
New-Item -Path "HKLM:\SOFTWARE\Classes\$progId" -Force | Out-Null
Set-ItemProperty -Path "HKLM:\SOFTWARE\Classes\$progId" -Name "(Default)" -Value "RAID Chart"
New-Item -Path "HKLM:\SOFTWARE\Classes\$progId\DefaultIcon" -Force | Out-Null
Set-ItemProperty -Path "HKLM:\SOFTWARE\Classes\$progId\DefaultIcon" -Name "(Default)" -Value "`"$IconPath`""

# Per-user registrations shadow HKLM for that user; refresh the shell anyway.
$sig = '[System.Runtime.InteropServices.DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);'
$shc = Add-Type -MemberDefinition $sig -Name SHCM$PID -Namespace WM$PID -PassThru
$shc::SHChangeNotify(0x08000000, 0, [System.IntPtr]::Zero, [System.IntPtr]::Zero)
ie4uinit.exe -ClearIconCache
Write-Output ".RAID -> $progId ($IconPath) [HKLM]"

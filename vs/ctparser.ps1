$FILE = Get-Content "FILTER.TXT"

class entry {
    [bool]$isCall
    [string]$caller_seg
    [string]$caller_off
    [string]$callee_seg
    [string]$callee_off
}

$hash = @{}

foreach ($LINE in $FILE) {

    # Write-Output "$LINE"
    $result = $LINE -match "(CALL|RET):\s+([0-9A-F]+):([0-9A-F]+) --> ([0-9A-F]+):([0-9A-F]+).*"
    $currentEntry = [entry]::new()

    if ($Matches[1] -eq "CALL") {
        $currentEntry.isCall = $true
    } else {
        $currentEntry.isCall = $false
    }
    $currentEntry.caller_seg = $Matches[2]
    $currentEntry.caller_off = $Matches[3]
    $currentEntry.callee_seg = $Matches[4]
    $currentEntry.callee_off = $Matches[5]

    if ($currentEntry.isCall) {
        $e = "$($currentEntry.caller_seg):$($currentEntry.caller_off)-$($currentEntry.callee_seg):$($currentEntry.callee_off)"
        if ($hash.Keys -contains $e) {
            $count = $hash[$e]
            $hash[$e] = $count + 1
        } else {
            $hash[$e] = 1
        }
    }
}

$hash.GetEnumerator() | Sort-Object -Property value

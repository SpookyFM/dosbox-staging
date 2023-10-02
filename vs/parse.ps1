
function HandleComparison {
    param (
        $list
    )
    
    # Iterate over the keys of the first one
    foreach ($address in $list[0].Keys) {
        # Write-Host "${h}: $($hash.$h)"
        
        $currentValues = @()
        foreach ($values in $list) {
            if ($values.Keys -contains $address) {
                $currentValue = $values[$address]
                # Filter for all below 255, I don't think the mode would be written as an actual word
                # TODO: If this does not yield something, reconsider this assumption
                if ($currentValue -le 255) {
                    $currentValues += ,$currentValue
                }
                
            }
        }
        $unique = $currentValues | Sort-Object -Unique
        if ($unique.Length -eq $list.Length) {
            Write-Host $address
            Write-Host $unique
        }
    }
}


function ParseLog {
    param (
        $filename
    )

    $values = @{}

    $file = Get-Content $filename

    $pattern = '([A-F0-9]{4}):([A-F0-9]{4}) (\d) ([A-F0-9]*) ([A-F0-9]*)'
    $regex = [regex]::new($pattern, "Compiled, CultureInvariant"); 

    Write-Host "Starting to read $filename"
    foreach ($line in $file) {
        $result = $regex.Matches($line)
        $seg = [Int32]("0x" + $result[0].Groups[1].Value)
        $off = [Int32]("0x" + $result[0].Groups[2].Value)
        $size = [Int32]("0x" + $result[0].Groups[3].Value)
        $value = [Int32]("0x" + $result[0].Groups[4].Value)
        $address = [Int32]("0x" + $result[0].Groups[5].Value)
        $values[$address] = $value
    }

    Write-Host "Finished reading $filename"
    return $values
}

# Next steps
# Ignore that this thing is slow as ...
# Parse the others as well
# Go over the keys in the first
# Check if they are in the others as well
# Output key and value in case they differ for any of them

$talk = ParseLog -filename ".\TALK.TXT"
$look = ParseLog -filename ".\LOOK.TXT"
$point = ParseLog -filename ".\POINT.TXT"
$cross = ParseLog -filename ".\CROSS.TXT"

$list = $talk,$look,$point,$cross

HandleComparison($list)
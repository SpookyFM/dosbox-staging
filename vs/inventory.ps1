# Define the path to your binary file
$filePath = "F:\Projects\dosbox-staging\vs\memdump.bin"

# Read the binary file as bytes
$fileBytes = [System.IO.File]::ReadAllBytes($filePath)

# Initialize an empty array to store 16-bit values
$bit16Array = @()

# Loop through the bytes in pairs and convert to 16-bit values
for ($i = 0; $i -lt $fileBytes.Length; $i += 2) {
    $value = [BitConverter]::ToInt16($fileBytes, $i)
    $bit16Array += $value
}

# Output the array of 16-bit values
$bit16Array
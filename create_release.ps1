# Create release directory with today's date
# Run from VNTranslationTools directory
$date = Get-Date -Format "MMMdd"
$releaseName = "VNTranslationTools_${date}_release"
$releaseDir = "..\$releaseName"

# Create the release directory
Write-Host "Creating release directory: $releaseDir"
New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null

# Copy winmm.dll
Write-Host "Copying winmm.dll..."
Copy-Item "Release\winmm.dll" -Destination $releaseDir

# Copy all TTF files from Fonts directory
Write-Host "Copying font files..."
Copy-Item "Fonts\*.ttf" -Destination $releaseDir

# Create VNTextPatch subdirectory and copy Build\Release contents
$vnTextPatchDir = Join-Path $releaseDir "VNTextPatch"
Write-Host "Creating VNTextPatch subdirectory and copying files..."
New-Item -ItemType Directory -Path $vnTextPatchDir -Force | Out-Null
Copy-Item "Build\Release\*" -Destination $vnTextPatchDir -Recurse

# Create zip file inside the release directory
$zipPath = Join-Path $releaseDir "VNTranslationTools_SoftPal.zip"
Write-Host "Creating zip file: $zipPath"
if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}
Compress-Archive -Path "$releaseDir\*" -DestinationPath $zipPath

Write-Host "Release created successfully!"
Write-Host "  Directory: $releaseDir"
Write-Host "  Zip file: $zipPath"

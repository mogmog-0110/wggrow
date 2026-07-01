# Fetch the two redistributable SDKs wggrow needs (not committed to keep the repo small):
#   - D3D12 Agility SDK 1.615.0  (Work Graphs runtime + headers)
#   - DirectX Shader Compiler 1.8.2505.28  (compiles shaders to DXIL SM6.8)
# Run once:  powershell -ExecutionPolicy Bypass -File fetch_deps.ps1
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
New-Item -ItemType Directory -Force -Path "$root\third_party" | Out-Null

function Fetch($pkg, $ver, $dest) {
    $zip = "$root\third_party\$pkg.zip"
    Write-Host "downloading $pkg $ver ..."
    Invoke-WebRequest -Uri "https://www.nuget.org/api/v2/package/$pkg/$ver" -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $dest -Force
    Remove-Item $zip
}
Fetch "Microsoft.Direct3D.D3D12" "1.615.0"     "$root\third_party\agility"
Fetch "Microsoft.Direct3D.DXC"   "1.8.2505.28" "$root\third_party\dxc"
Write-Host "done. now run build.bat from an x64 Native Tools Command Prompt."

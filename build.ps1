param(
    [string]$Zig = "zig",
    [string]$Out = "bin/DshPet.exe"
)
$ErrorActionPreference = "Stop"
& $Zig c++ -target x86_64-windows-gnu -O2 -std=c++17 -static -Wl,--subsystem,windows `
    src/pet.cpp -o $Out -lgdiplus -lshell32 -luser32 -lgdi32 -lole32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
"built $Out"

# Smart Receiver - ESPHome build helper (Windows PowerShell)
# Usage (from this folder):
#   powershell -ExecutionPolicy Bypass -File .\build.ps1            # compile
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 run        # compile + upload (OTA or COM port prompt)
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 upload --device COM7
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 logs --device COM7
#
# ESP-IDF (ESPHome-managed 5.5.x) and tools live in C:\esphome-idf: no spaces,
# and kept separate from your own ESP-IDF 6.1 install in ~\.espressif.

param(
    [string]$Command = "compile",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# Never pick up an activated ESP-IDF 6.1 environment.
Remove-Item Env:IDF_PATH -ErrorAction SilentlyContinue
Remove-Item Env:IDF_PYTHON_ENV_PATH -ErrorAction SilentlyContinue
Remove-Item Env:XTENSA_GNU_CONFIG -ErrorAction SilentlyContinue

$env:ESPHOME_ESP_IDF_PREFIX = "C:\esphome-idf"
$env:IDF_TOOLS_PATH = "C:\esphome-idf\tools"

$esphome = Join-Path $PSScriptRoot ".venv\Scripts\esphome.exe"
if (-not (Test-Path $esphome)) {
    throw "ESPHome not found in .venv. Run: py -m venv .venv; .\.venv\Scripts\python.exe -m pip install esphome"
}

& $esphome --toolchain esp-idf $Command yamaha-rxv540.yaml @Rest
exit $LASTEXITCODE

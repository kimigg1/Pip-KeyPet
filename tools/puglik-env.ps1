# Окружение puglik для PowerShell на Windows. Запуск из корня проекта:  . .\tools\puglik-env.ps1
# Добавляет в PATH этого окна: Python 3.12 (для ESP-IDF), GCC/CMake/Ninja из WinLibs (для симулятора)
# и подключает ESP-IDF (idf.py). Ничего не меняет в системе.

$py312 = Join-Path $env:LOCALAPPDATA "Python\pythoncore-3.12-64"
if (-not (Test-Path "$py312\python.exe")) { Write-Warning "Нет Python 3.12: py install 3.12" }

$winlibs = Get-ChildItem (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages") -Directory -Filter "BrechtSanders.WinLibs*" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($winlibs) { $gccBin = Join-Path $winlibs.FullName "mingw64\bin" } else { Write-Warning "Нет WinLibs: winget install BrechtSanders.WinLibs.POSIX.UCRT"; $gccBin = "" }

$env:PATH = "$py312;$gccBin;" + $env:PATH

$idf = Join-Path $env:USERPROFILE "esp\esp-idf\export.ps1"
if (Test-Path $idf) { . $idf *> $null } else { Write-Warning "Нет ESP-IDF в $idf" }

Write-Host "puglik: gcc, cmake, ninja и idf.py готовы в этом окне"

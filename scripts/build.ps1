# Obter o hash do commit atual do vcpkg
$hash = (git -C .\dep\vcpkg rev-parse HEAD)

# Atualizar ou criar o arquivo vcpkg.json com o hash
if (Test-Path vcpkg.json) {
    $json = Get-Content -Raw vcpkg.json | ConvertFrom-Json
    $json.'builtin-baseline' = $hash
    $json | ConvertTo-Json -Depth 10 | Set-Content vcpkg.json
} else {
    @{ 'builtin-baseline' = $hash } | ConvertTo-Json | Set-Content vcpkg.json
}

# Verificar se a atualização do vcpkg.json foi bem sucedida
if ($LASTEXITCODE -ne 0) {
    Write-Error "Falha ao atualizar vcpkg.json"
    exit 1
}

# Bootstrap vcpkg
Write-Host "Executando bootstrap do vcpkg..." -ForegroundColor Cyan
& '.\dep\vcpkg\bootstrap-vcpkg.bat'
if ($LASTEXITCODE -ne 0) {
    Write-Error "Falha no bootstrap do vcpkg"
    exit 1
}

# Instalar dependências
Write-Host "Instalando dependências..." -ForegroundColor Cyan
& '.\dep\vcpkg\vcpkg' install --triplet x86-windows-static
if ($LASTEXITCODE -ne 0) {
    Write-Error "Falha na instalação das dependências"
    exit 1
}

# Configurar CMake
Write-Host "Configurando CMake..." -ForegroundColor Cyan
cmake -B build -G "Visual Studio 17 2022" -A Win32 `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE='./dep/vcpkg/scripts/buildsystems/vcpkg.cmake' `
    -DVCPKG_TARGET_TRIPLET=x86-windows-static
if ($LASTEXITCODE -ne 0) {
    Write-Error "Falha na configuração do CMake"
    exit 1
}

# Compilar o projeto
Write-Host "Compilando o projeto..." -ForegroundColor Cyan
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    Write-Error "Falha na compilação"
    exit 1
}

Write-Host "Build concluído com sucesso!" -ForegroundColor Green

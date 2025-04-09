# Servidor de Depuração SourcePawn

![SM-Ext](https://github.com/ViniciusRed/sm-debugger/actions/workflows/cmake.yml/badge.svg)
![VsCode](https://github.com/ViniciusRed/sm-debugger/actions/workflows/vscode.yml/badge.svg)

### Descrição
Este é um servidor de depuração para SourceMod que permite a depuração remota de scripts SourcePawn. Atualmente, inclui um adaptador para VSCode, permitindo integração com o Visual Studio Code.

### Dependências
- **Máquina Virtual SourcePawn**: Esta extensão requer a VM do SourcePawn para funcionar.
  - Ao baixar o releases: O VM do SourcePawn já está incluída no pacote do release
  - Para compilar a VM do SourcePawn:
    1. Clone o repositório do SourcePawn:
       ```bash
       git clone --recursive https://github.com/alliedmodders/sourcemod.git sourcemod
       cd sourcemod/sourcepawn
       ```
    2. Troque para a branch com suporte a símbolos de depuração:
       ```bash
       git remote add peace https://github.com/peace-maker/sourcepawn.git
       git fetch peace
       git switch debug_api_symbols
       git submodule update --init --recursive
       ```
    3. Instale as dependências necessárias:
       **Linux:**
       ```bash
       sudo dpkg --add-architecture i386
       sudo apt-get update
       sudo apt-get install -y --no-install-recommends \
         gcc-multilib g++-multilib libstdc++6 lib32stdc++6 \
         libc6-dev libc6-dev-i386 linux-libc-dev \
         linux-libc-dev:i386 clang
       ```
       
       Em seguida, configure as variáveis de ambiente:
       ```bash
       export CC=clang
       export CXX=clang++
       ```

       **Para todos os sistemas:**
       ```bash
       python -m pip install wheel
       pip install git+https://github.com/alliedmodders/ambuild
       ```

    4. Configure e compile:
       ```bash
       python configure.py --enable-optimize --targets x86,x86_64
       ambuild objdir
       ```

    5. Os arquivos compilados estarão em:
       **Linux:**
       - 32-bit: `objdir/libsourcepawn/linux-x86/libsourcepawn.so`
       - 64-bit: `objdir/libsourcepawn/linux-x86_64/libsourcepawn.so`
       
       **Windows:**
       - 32-bit: `objdir/libsourcepawn/windows-x86/libsourcepawn.dll`
       - 64-bit: `objdir/libsourcepawn/windows-x86_64/libsourcepawn.dll`

    6. Copie os arquivos para sua instalação do SourceMod:
       - 32-bit: `addons/sourcemod/bin/sourcepawn.jit.x86.{so|dll}`
       - 64-bit: `addons/sourcemod/bin/x64/sourcepawn.vm.{so|dll}`

### Funcionalidades
- Suporte para depuração remota de scripts SourcePawn
- Integração com Visual Studio Code
- Capacidades de depuração em tempo real
- Suporte para breakpoints e inspeção de variáveis

### Instalação e Configuração

1. **Obter a Extensão**
   - Clone este repositório e compile
   - OU baixe a última versão do GitHub

2. **Instalação**
   - Copie o arquivo da extensão para `addons/sourcemod/extensions/`
   - Crie um arquivo chamado `sm_debugger.autoload` no mesmo diretório

3. **Configuração do Servidor**
   - Inicie seu Servidor Dedicado Source (SRCDS)
   - Certifique-se de que a extensão foi carregada corretamente

4. **Integração com VSCode**
   - Instale a extensão VSCode SourcePawn Debug
   - Siga as instruções de configuração em [sm-debugger](https://github.com/ViniciusRed/sm-debugger)

### Compilando do Código Fonte

#### Extensão do Depurador (sm_debugger)

**Dependências Linux:**
```bash
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  gcc-multilib g++-multilib libstdc++6 lib32stdc++6 \
  libc6-dev libc6-dev-i386 linux-libc-dev \
  linux-libc-dev:i386 ninja-build cmake
```

**Compilando no Linux:**
```bash
# Instale as dependências do vcpkg
cp cmake/x86-linux-sm.cmake dep/vcpkg/triplets/x86-linux-sm.cmake
./dep/vcpkg/bootstrap-vcpkg.sh -musl
./dep/vcpkg/vcpkg install --triplet x86-linux-sm --debug

# Configure e compile
rm -rf build
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="./dep/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x86-linux-sm \
  -DCMAKE_CXX_FLAGS_INIT="-m32" \
  -DCMAKE_C_FLAGS_INIT="-m32"
cmake --build build -j
```

**Compilando no Windows:**
```bash
# Instale as dependências do vcpkg
./dep/vcpkg/bootstrap-vcpkg.bat -disableMetrics
./dep/vcpkg/vcpkg install --triplet x86-windows-static

# Configure e compile
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="./dep/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x86-windows-static
cmake --build build --config Release --parallel
```

O arquivo compilado será gerado em `build/sm_debugger.ext.so` (Linux) ou `build/sm_debugger.ext.dll` (Windows).

#### Extensão VSCode

Para compilar a extensão do VSCode:

```bash
# Entre no diretório da extensão
cd vscode

# Instale as dependências
npm install

# Compile a extensão
npm run compile

# Para desenvolvimento com recompilação automática
npm run watch

# Para criar o pacote VSIX
npx vsce package
```

O arquivo VSIX será gerado no diretório `vscode/`.

## TODO
- [x] Suporte para Windows
- [x] Integração com VSCode
- [ ] Recursos de depuração aprimorados
- [ ] Testes e validação no Linux

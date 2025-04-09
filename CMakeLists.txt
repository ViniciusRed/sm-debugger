cmake_minimum_required(VERSION 3.18)

project(sm_debugger)

# Encontrar dependências
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(fmt CONFIG REQUIRED)

# Configuração do SDK
if(NOT SDK)
    set(SDK csgo)
endif()

# Configuração do nome de saída
set(OUTPUT_NAME "sm_debugger" CACHE STRING "Output name of the extension")
string(REGEX REPLACE " " "" OUTPUT_NAME "${OUTPUT_NAME}")
if(OUTPUT_NAME STREQUAL "")
    message(FATAL_ERROR "OUTPUT_NAME must be configured")
endif()

# Caminhos para SourceMod e SourcePawn
set(SM_PATH "${CMAKE_CURRENT_SOURCE_DIR}/dep/sourcemod")
set(SP_PATH "${CMAKE_CURRENT_SOURCE_DIR}/dep/sourcemod/sourcepawn")

# Incluir arquivos auxiliares
include("cmake/sourcemod.cmake")
include("cmake/cmake-version4git.cmake")

# Obter versão do Git
PROJECT_VERSION_FROM_GIT()

# Verificar arquitetura 32-bit
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Arch must be 32-bit")
endif()

# Lista de arquivos fonte
set(CPP_FILES 
    "src/sourcepawn/vm/smx-v1-image.cpp"
    "src/sourcepawn/vm/file-utils.cpp"
    "src/sourcepawn/vm/rtti.cpp"
    "src/extension.cpp"
    "src/debugger.cpp"
    "src/utlbuffer.cpp"
)

# Agrupar arquivos para IDE
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CPP_FILES})

# Adicionar o alvo (extensão)
add_extension(${OUTPUT_NAME} ${CPP_FILES})

# Configurações específicas do compilador
if(MSVC)
    target_compile_options(${OUTPUT_NAME} PRIVATE /utf-8 /W3)
    target_compile_definitions(${OUTPUT_NAME} PRIVATE 
        WIN32 
        _WINDOWS 
        COMPILER_MSVC 
        COMPILER_MSVC32
    )
else()
    target_compile_options(${OUTPUT_NAME} PRIVATE -m32)
    target_compile_definitions(${OUTPUT_NAME} PRIVATE 
        _LINUX 
        POSIX
    )
endif()

if(CMAKE_COMPILER_IS_GNUCXX)
    target_compile_definitions(${OUTPUT_NAME} PRIVATE 
        COMPILER_GCC 
        _vsnprintf=vsnprintf 
        _snprintf=snprintf 
        _stricmp=strcasecmp 
        stricmp=strcasecmp
    )
endif()

# Linkar bibliotecas
target_link_libraries(${OUTPUT_NAME} PRIVATE 
    ZLIB::ZLIB 
    fmt::fmt-header-only 
    nlohmann_json::nlohmann_json
)
if(MSVC)
    target_link_libraries(${OUTPUT_NAME} PRIVATE 
        ws2_32 
        legacy_stdio_definitions
    )
endif()

# Definir padrão C++17
set_target_properties(${OUTPUT_NAME} PROPERTIES 
    CXX_STANDARD 17
    CXX_EXTENSIONS ON
)

# Diretórios de include
target_include_directories(${OUTPUT_NAME} PRIVATE 
    ${ZLIB_INCLUDE_DIR}
)
target_include_directories(${OUTPUT_NAME} PUBLIC 
    "src"
    "dep/certify/include"
    "src/sourcepawn/include"
    "src/sourcepawn/vm"
    "dep/sourcemod/public/amtl"
)

# Adicionar definições de versão do Git
target_compile_definitions(${OUTPUT_NAME} PRIVATE ${PROJECT_VERSION4GIT_CFLAGS})

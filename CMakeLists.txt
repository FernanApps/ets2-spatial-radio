cmake_minimum_required(VERSION 3.21)
project(ETS2CabinRadio VERSION 1.2.1 LANGUAGES C CXX)

# La DLL de procesamiento de audio que se inyecta en ETS2
add_library(ETS2CabinDSP SHARED LiveCabinDSP.c)

# Exportar las funciones del plugin SCS
target_compile_definitions(ETS2CabinDSP PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    # Optimización y warnings
    target_compile_options(ETS2CabinDSP PRIVATE /O2 /W4)
    # Sin CRT para DLL más pequeña (opcional, comentar si da problemas)
    # target_link_options(ETS2CabinDSP PRIVATE /NODEFAULTLIB)
else()
    target_compile_options(ETS2CabinDSP PRIVATE -O2 -Wall -Wextra)
endif()

# Herramienta de prueba (solo genera tono, no necesita pyaudio)
add_executable(bridge_test tools/bridge_test.c)
if(MSVC)
    target_compile_options(bridge_test PRIVATE /O2 /W4)
else()
    target_compile_options(bridge_test PRIVATE -O2 -Wall -Wextra)
endif()

# Capturador de audio por proceso (WASAPI Process Loopback, Windows 10 2004+).
# Modo bridge: escribe en Local\ETS2CabinBridge. Modo --test: graba WAV sin DLL.
if(WIN32)
    add_executable(capture_process tools/capture_process.cpp)
    target_include_directories(capture_process PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(capture_process PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(capture_process PRIVATE /O2 /W4)
    else()
        target_compile_options(capture_process PRIVATE -O2 -Wall -Wextra)
    endif()
    target_link_libraries(capture_process PRIVATE Mmdevapi Ole32 Avrt)
endif()

# Copiar la DLL a la carpeta de plugins de ETS2 después de compilar (opcional)
# Descomenta y ajusta la ruta si quieres que se copie automáticamente:
# add_custom_command(TARGET ETS2CabinDSP POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ETS2CabinDSP>
#     "C:/Program Files (x86)/Steam/steamapps/common/Euro Truck Simulator 2/bin/win_x64/plugins/"
# )

message(STATUS "ETS2 Cabin Radio v${PROJECT_VERSION}")
message(STATUS "  DLL: ETS2CabinDSP.dll")
message(STATUS "  Test: bridge_test.exe")
if(WIN32)
    message(STATUS "  Capture: capture_process.exe")
endif()

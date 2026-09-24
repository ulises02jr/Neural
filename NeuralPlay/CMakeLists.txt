cmake_minimum_required(VERSION 3.22)
project(NeuralPlaySpike VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(JUCE)

# ─────────────────────────────────────────────────────────────
# Opciones específicas de plataforma.
# En macOS (Unix Makefiles) este bloque queda vacío y todo sigue igual.
# En iOS/iPad se activan las opciones nativas de JUCE.
# ─────────────────────────────────────────────────────────────
set(NP_PLATFORM_OPTS "")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Firma y versión mínima de iOS (Xcode ya no acepta menos de 15.0)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "iOS deployment target" FORCE)
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "B9JRFBT9PG")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic")
    set(NP_PLATFORM_OPTS
        BUNDLE_ID                        com.neuralworship.play
        BACKGROUND_AUDIO_ENABLED         TRUE
        REQUIRES_FULL_SCREEN             TRUE
        TARGETED_DEVICE_FAMILY           "1,2"   # 1 = iPhone, 2 = iPad
        IPAD_SCREEN_ORIENTATIONS         UIInterfaceOrientationLandscapeLeft
                                         UIInterfaceOrientationLandscapeRight
        IPHONE_SCREEN_ORIENTATIONS       UIInterfaceOrientationLandscapeLeft
                                         UIInterfaceOrientationLandscapeRight
        MICROPHONE_PERMISSION_ENABLED    FALSE
        LOCAL_NETWORK_PERMISSION_ENABLED TRUE
        LOCAL_NETWORK_PERMISSION_TEXT    "NeuralPlay usa la red local para que los musicos se conecten al modo En Vivo."
    )
endif()

# Icono por plataforma:
#  - iOS: opaco a sangre completa (iOS le aplica su propia máscara redondeada).
#  - macOS: el squircle YA redondeado con transparencia (macOS no redondea solo),
#    para que en el Dock/Launchpad se vea igual que en iPhone/iPad.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(NP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/AppIconOpaque.png")
else()
    set(NP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png")
endif()

juce_add_gui_app(NeuralPlaySpike
    PRODUCT_NAME "NeuralPlay"
    ICON_BIG "${NP_ICON}"
    ${NP_PLATFORM_OPTS})

juce_add_binary_data(NeuralPlayData SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/LogoInterno.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/SpaceGrotesk-Regular.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/SpaceGrotesk-Bold.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Inter-Regular.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Inter-Bold.ttf")

juce_generate_juce_header(NeuralPlaySpike)

target_sources(NeuralPlaySpike PRIVATE Source/Main.cpp)

target_compile_definitions(NeuralPlaySpike PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:NeuralPlaySpike,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:NeuralPlaySpike,JUCE_VERSION>")

target_link_libraries(NeuralPlaySpike
    PRIVATE
        NeuralPlayData
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

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
    set(NP_PLATFORM_OPTS
        BUNDLE_ID                        com.neuralworship.play
        BACKGROUND_AUDIO_ENABLED         TRUE
        REQUIRES_FULL_SCREEN             TRUE
        TARGETED_DEVICE_FAMILY           "2"   # 2 = iPad
        IPAD_SCREEN_ORIENTATIONS         UIInterfaceOrientationLandscapeLeft
                                         UIInterfaceOrientationLandscapeRight
        MICROPHONE_PERMISSION_ENABLED    FALSE
        LOCAL_NETWORK_PERMISSION_ENABLED TRUE
        LOCAL_NETWORK_PERMISSION_TEXT    "NeuralPlay usa la red local para que los musicos se conecten al modo En Vivo."
    )
endif()

juce_add_gui_app(NeuralPlaySpike
    PRODUCT_NAME "NeuralPlay"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png"
    ${NP_PLATFORM_OPTS})

juce_add_binary_data(NeuralPlayData SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png")

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

cmake_minimum_required(VERSION 3.22)
project(NeuralPlaySpike VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(JUCE)

juce_add_gui_app(NeuralPlaySpike
    PRODUCT_NAME "NeuralPlay"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png")
juce_generate_juce_header(NeuralPlaySpike)

target_sources(NeuralPlaySpike PRIVATE Source/Main.cpp)

target_compile_definitions(NeuralPlaySpike PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:NeuralPlaySpike,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:NeuralPlaySpike,JUCE_VERSION>")

target_link_libraries(NeuralPlaySpike
    PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

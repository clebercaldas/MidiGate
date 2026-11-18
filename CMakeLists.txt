# ==============================================================================
#  MidiGate - CMakeLists.txt
#
#  Compila o plugin como Audio Unit (AU) e VST3 no macOS.
#  O JUCE é baixado automaticamente na primeira configuração (FetchContent).
#  Para usar uma cópia local do JUCE, passe:
#      -DFETCHCONTENT_SOURCE_DIR_JUCE=/caminho/para/JUCE
# ==============================================================================

cmake_minimum_required(VERSION 3.22)

# --- Configurações específicas do macOS (precisam vir ANTES do project()) -----
# Binário universal: roda nativamente em Macs Intel e Apple Silicon.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Arquiteturas do macOS")
# Versão mínima do macOS suportada pelo plugin.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Versão mínima do macOS")

project(MidiGate VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Formatos gerados. AU só existe no macOS; em outros sistemas geramos só VST3.
if(APPLE)
    set(MIDIGATE_FORMATS AU VST3)
else()
    set(MIDIGATE_FORMATS VST3)
endif()

# --- Download do JUCE ---------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# --- Definição do plugin ------------------------------------------------------
juce_add_plugin(MidiGate
    COMPANY_NAME            "RegionalRJ"
    PRODUCT_NAME            "MidiGate"
    BUNDLE_ID               "com.regionalrj.midigate"

    # Códigos de 4 caracteres. O do fabricante precisa ter ao menos uma
    # letra maiúscula. Troque por códigos próprios antes de distribuir.
    PLUGIN_MANUFACTURER_CODE Rgrj
    PLUGIN_CODE              Mgat

    FORMATS                 ${MIDIGATE_FORMATS}

    # É um efeito de áudio (não sintetizador) que RECEBE MIDI.
    # Com essa combinação o AU é registrado como "kAudioUnitType_MusicEffect"
    # (aumf), o tipo que o Logic Pro chama de "MIDI-controlled effect" e que
    # aceita sidechain MIDI.
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        TRUE
    NEEDS_MIDI_OUTPUT       FALSE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE

    # Categorias exibidas pela DAW.
    AU_MAIN_TYPE            kAudioUnitType_MusicEffect
    VST3_CATEGORIES         Fx Dynamics

    # Copia o plugin para ~/Library/Audio/Plug-Ins/{Components,VST3}
    # automaticamente após cada build.
    COPY_PLUGIN_AFTER_BUILD TRUE
)

target_sources(MidiGate
    PRIVATE
        Source/PluginProcessor.cpp
)

target_compile_definitions(MidiGate
    PUBLIC
        # Desliga módulos que não usamos (binário menor, menos dependências).
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(MidiGate
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

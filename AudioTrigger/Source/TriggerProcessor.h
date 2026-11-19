/*
  ==============================================================================

    AudioTrigger - TriggerProcessor.h

    Detector de transientes em TEMPO REAL que envia MIDI.

    IDEIA GERAL
    -----------
    É o inverso do MidiGate:

        MidiGate     : MIDI entra  ->  controla o volume do áudio
        AudioTrigger : áudio entra ->  gera MIDI (Note On / Note Off)

    Quando o sinal de áudio passa de um limiar (threshold) o "gate abre" e o
    plugin envia um Note On. Quando o sinal cai abaixo do limiar de fechamento,
    envia o Note Off. O áudio passa pelo plugin sem nenhuma alteração.

    PARA QUE SERVE
    --------------
    Tocar um instrumento (bateria sampleada, sintetizador...) a partir de um
    áudio ao vivo, por exemplo o microfone ou o pad de um baterista.

    DOIS CAMINHOS PARA O MIDI SAIR DO PLUGIN
    ----------------------------------------
    1. Saída MIDI do próprio plugin (MidiBuffer). Funciona em DAWs que
       encaminham a saída MIDI de plugins para outras trilhas (ex.: Reaper).

    2. Porta MIDI virtual (CoreMIDI no macOS). O plugin cria uma porta chamada
       "AudioTrigger Out". Uma trilha de instrumento do Logic Pro pode
       receber MIDI dela. Este caminho existe porque o Logic não encaminha a
       saída MIDI de plugins de áudio para outras trilhas.

    REGRAS DE OURO DA THREAD DE ÁUDIO (processBlock)
    ------------------------------------------------
    - Sem locks, sem alocação de memória e sem chamadas de sistema.
    - O envio para a porta virtual (que é uma chamada de sistema) NÃO é feito
      na thread de áudio. O processBlock só coloca o evento numa fila sem lock
      (juce::AbstractFifo) e uma thread separada faz o envio de fato.

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>

//==============================================================================
namespace TriggerParamIDs
{
    inline constexpr const char* threshold  = "threshold";   // dB: abre o gate
    inline constexpr const char* hysteresis = "hysteresis";  // dB abaixo do threshold: fecha
    inline constexpr const char* release    = "release";     // ms: queda do detector
    inline constexpr const char* lockout    = "lockout";     // ms: tempo mínimo entre disparos
    inline constexpr const char* velWindow  = "velwindow";   // ms: janela para medir a força
    inline constexpr const char* note       = "note";        // nota MIDI enviada
    inline constexpr const char* channel    = "channel";     // canal MIDI (1..16)
    inline constexpr const char* velocity   = "velocity";    // 0 = dinâmica, 1..127 = fixa
    inline constexpr const char* port       = "port";        // enviar à porta virtual?
}

//==============================================================================
/**
    Porta MIDI virtual + thread de envio.

    A thread de áudio chama push() (sem lock). Uma thread própria esvazia a
    fila a cada ~1 ms e envia as mensagens ao sistema.
*/
class VirtualMidiPort final : private juce::Thread
{
public:
    explicit VirtualMidiPort (const juce::String& portName);
    ~VirtualMidiPort() override;

    /** true se o sistema permitiu criar a porta (macOS e Linux sim; Windows não). */
    bool isOpen() const noexcept { return output != nullptr; }

    /** Chamado na thread de áudio. Sem lock e sem alocação. */
    void push (juce::uint8 status, juce::uint8 data1, juce::uint8 data2) noexcept;

private:
    void run() override;
    void drain();

    struct Event { juce::uint8 bytes[3] {}; };

    static constexpr int fifoCapacity = 256;

    std::unique_ptr<juce::MidiOutput> output;
    juce::AbstractFifo fifo { fifoCapacity };
    std::array<Event, (size_t) fifoCapacity> events {};
};

//==============================================================================
class TriggerAudioProcessor final : public juce::AudioProcessor
{
public:
    TriggerAudioProcessor();
    ~TriggerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return true;  }   // este plugin GERA MIDI
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override                               { return 1; }
    int getCurrentProgram() override                            { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const juce::String&) override  {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==========================================================================
    // Ponteiros para os valores atômicos dos parâmetros (leitura sem lock).
    std::atomic<float>* thresholdParam  = nullptr;
    std::atomic<float>* hysteresisParam = nullptr;
    std::atomic<float>* releaseParam    = nullptr;
    std::atomic<float>* lockoutParam    = nullptr;
    std::atomic<float>* velWindowParam  = nullptr;
    std::atomic<float>* noteParam       = nullptr;
    std::atomic<float>* channelParam    = nullptr;
    std::atomic<float>* velocityParam   = nullptr;
    std::atomic<float>* portParam       = nullptr;

    //==========================================================================
    // Máquina de estados do detector.
    //
    //   idle       : esperando o sinal passar do threshold
    //   measuring  : passou do threshold; medindo o pico por alguns ms para
    //                decidir a velocity (só quando a velocity é dinâmica)
    //   open       : Note On enviado; esperando o sinal cair para enviar Note Off
    enum class State { idle, measuring, open };

    State  state          = State::idle;
    double sampleRate     = 44100.0;
    float  envelope       = 0.0f;   // seguidor de pico (0..1)
    float  windowPeak     = 0.0f;   // maior valor visto na janela de medição
    int    windowLeft     = 0;      // amostras restantes da janela
    int    lockoutLeft    = 0;      // amostras restantes do bloqueio
    int    heldNote       = -1;     // nota do Note On atual (para o Note Off)
    int    heldChannel    = 1;

    // Pedido para fechar uma nota presa (ex.: o transporte parou). Pode ser
    // feito por outra thread, por isso é atômico.
    std::atomic<bool> pendingReset { false };

    //==========================================================================
    // Porta virtual (pode ser nula se o sistema não permitir).
    std::unique_ptr<VirtualMidiPort> virtualPort;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TriggerAudioProcessor)
};

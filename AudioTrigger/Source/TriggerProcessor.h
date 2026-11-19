/*
  ==============================================================================

    AudioTrigger - TriggerProcessor.h

    Gate de áudio em TEMPO REAL que também ENVIA MIDI ao abrir e ao fechar.
    O modelo é o ReaGate do Reaper (que não existe no Logic Pro).

    IDEIA GERAL
    -----------
    O sinal de entrada passa por um detector. Quando o nível passa do
    "Threshold" o gate abre; quando cai abaixo de "Threshold - Hysteresis" (e o
    tempo de Hold termina) o gate fecha.

        abre   -> áudio sobe (Attack)  + MIDI Note On  (nota configurável)
        fecha  -> áudio desce (Release) + MIDI Note Off

    CAMINHO DO SINAL
    ----------------

        Entrada ─┬─► [Lowpass/Highpass] ─► [Peak ou RMS] ─► Gate ─┐
                 │                                                 │ ganho 0..1
                 └─► [Pre-open: atraso] ──► Wet x ganho + Dry ─────┴─► Saída
                                            + Noise x ganho

    O detector vê o sinal SEM atraso e o áudio que sai está atrasado em
    "Pre-open" ms. Por isso o gate já está aberto quando o golpe chega à saída.

    DOIS CAMINHOS PARA O MIDI SAIR DO PLUGIN
    ----------------------------------------
    1. Saída MIDI do próprio plugin (MidiBuffer). Funciona em DAWs que
       encaminham a saída MIDI de plugins (ex.: Reaper).

    2. Porta MIDI virtual (CoreMIDI no macOS) chamada "AudioTrigger Out".
       Uma trilha de instrumento do Logic Pro pode receber MIDI dela. Existe
       porque o Logic não encaminha a saída MIDI de plugins de áudio.

    REGRAS DE OURO DA THREAD DE ÁUDIO (processBlock)
    ------------------------------------------------
    - Sem locks, sem alocação de memória e sem chamadas de sistema.
    - Todos os buffers (atraso, RMS) são criados em prepareToPlay().
    - O envio para a porta virtual NÃO é feito na thread de áudio: o
      processBlock só coloca o evento numa fila sem lock (juce::AbstractFifo)
      e uma thread separada faz o envio.

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>
#include <vector>

//==============================================================================
namespace TriggerParamIDs
{
    // Detector e envelope (como no ReaGate)
    inline constexpr const char* threshold  = "threshold";   // dB
    inline constexpr const char* hysteresis = "hysteresis";  // dB abaixo do threshold: fecha
    inline constexpr const char* preOpen    = "preopen";     // ms de antecedência (atraso do áudio)
    inline constexpr const char* attack     = "attack";      // ms
    inline constexpr const char* hold       = "hold";        // ms
    inline constexpr const char* release    = "release";     // ms
    inline constexpr const char* lowpass    = "lowpass";     // Hz (filtro só do detector)
    inline constexpr const char* highpass   = "highpass";    // Hz (0 = desligado)
    inline constexpr const char* rmsSize    = "rms";         // ms (0 = detecção por pico)
    inline constexpr const char* preview    = "preview";     // ouvir o sinal filtrado do detector

    // Mix de saída
    inline constexpr const char* wet        = "wet";         // dB (áudio gateado)
    inline constexpr const char* dry        = "dry";         // dB (áudio direto)
    inline constexpr const char* noise      = "noise";       // dB (ruído com o gate aberto)

    // MIDI
    inline constexpr const char* sendMidi   = "sendmidi";    // enviar MIDI ao abrir/fechar
    inline constexpr const char* note       = "note";
    inline constexpr const char* channel    = "channel";     // 1..16
    inline constexpr const char* velocity   = "velocity";    // 0 = pela amplitude, 1..127 = fixa
    inline constexpr const char* velLow     = "vellow";      // dB do pico que vira velocity 1
    inline constexpr const char* velHigh    = "velhigh";     // dB do pico que vira velocity 127
    inline constexpr const char* velCurve   = "velcurve";    // 1 = linear em dB
    inline constexpr const char* velWindow  = "velwindow";   // ms para medir a força do golpe
    inline constexpr const char* lockout    = "lockout";     // ms mínimos entre dois disparos
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
class TriggerAudioProcessor final : public juce::AudioProcessor,
                                    private juce::Timer
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

    /** Roda na thread de mensagens (não é tempo real): informa à DAW a
        latência causada pelo Pre-open. */
    void timerCallback() override;

    //==========================================================================
    /** Filtro de 2ª ordem (Butterworth) só para o sinal do detector. */
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;

        void clear() noexcept { z1 = z2 = 0.0f; }

        float process (float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        void setLowpass (double sampleRate, double frequency);
        void setHighpass (double sampleRate, double frequency);
    };

    //==========================================================================
    // Ponteiros para os valores atômicos dos parâmetros (leitura sem lock).
    std::atomic<float>* thresholdParam  = nullptr;
    std::atomic<float>* hysteresisParam = nullptr;
    std::atomic<float>* preOpenParam    = nullptr;
    std::atomic<float>* attackParam     = nullptr;
    std::atomic<float>* holdParam       = nullptr;
    std::atomic<float>* releaseParam    = nullptr;
    std::atomic<float>* lowpassParam    = nullptr;
    std::atomic<float>* highpassParam   = nullptr;
    std::atomic<float>* rmsParam        = nullptr;
    std::atomic<float>* previewParam    = nullptr;
    std::atomic<float>* wetParam        = nullptr;
    std::atomic<float>* dryParam        = nullptr;
    std::atomic<float>* noiseParam      = nullptr;
    std::atomic<float>* sendMidiParam   = nullptr;
    std::atomic<float>* noteParam       = nullptr;
    std::atomic<float>* channelParam    = nullptr;
    std::atomic<float>* velocityParam   = nullptr;
    std::atomic<float>* velLowParam     = nullptr;
    std::atomic<float>* velHighParam    = nullptr;
    std::atomic<float>* velCurveParam   = nullptr;
    std::atomic<float>* velWindowParam  = nullptr;
    std::atomic<float>* lockoutParam    = nullptr;
    std::atomic<float>* portParam       = nullptr;

    //==========================================================================
    // Máquina de estados do gate.
    //
    //   closed    : fechado (o áudio está em silêncio ou em release)
    //   measuring : passou do threshold; medindo o pico por alguns ms para
    //               decidir a velocity (só quando há MIDI com velocity pela
    //               amplitude). O áudio já está abrindo.
    //   open      : aberto (inclui o tempo de Hold depois que o nível caiu)
    enum class State { closed, measuring, open };

    State  state       = State::closed;
    double sampleRate  = 44100.0;

    float  level       = 0.0f;   // nível do detector (pico ou RMS), 0..1
    float  gateGain    = 0.0f;   // ganho do áudio, 0 (fechado) .. 1 (aberto)
    float  windowPeak  = 0.0f;
    int    windowLeft  = 0;
    int    holdLeft    = 0;
    int    lockoutLeft = 0;
    int    heldNote    = -1;     // nota do Note On atual (para o Note Off)
    int    heldChannel = 1;

    // Filtros do detector (e os últimos valores aplicados, para só recalcular
    // os coeficientes quando o usuário mexer no knob).
    Biquad lowpassFilter, highpassFilter;
    float  lastLowpassHz = -1.0f, lastHighpassHz = -1.0f;

    // Janela RMS (soma móvel dos quadrados) com memória fixa.
    std::vector<float> rmsBuffer;
    double rmsSum        = 0.0;
    int    rmsPos        = 0;
    int    lastRmsLength = -1;

    // Atraso do Pre-open: um buffer circular por canal.
    std::array<std::vector<float>, 2> delayBuffer;
    int delayPos = 0;

    // Ganhos de saída suavizados (evitam estalo ao mexer nos knobs).
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetGain, dryGain, noiseGain;

    // Gerador de ruído (xorshift): leve e sem alocação.
    juce::uint32 noiseState = 22222u;

    // Pedido para zerar o estado e fechar a nota presa (ex.: transporte
    // parou). Pode vir de outra thread, por isso é atômico.
    std::atomic<bool> pendingReset { false };

    //==========================================================================
    std::unique_ptr<VirtualMidiPort> virtualPort;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TriggerAudioProcessor)
};

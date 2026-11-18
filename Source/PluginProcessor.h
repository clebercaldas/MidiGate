/*
  ==============================================================================

    MidiGate - PluginProcessor.h

    Gate de áudio acionado por MIDI (MIDI-triggered gate).

    IDEIA GERAL
    -----------
    Um "gate" comum abre e fecha de acordo com o volume do próprio áudio.
    Aqui quem manda é o MIDI:

        Note On  -> o gate ABRE   (ganho sobe suavemente até 1.0)
        Note Off -> o gate FECHA  (ganho desce suavemente até o "Floor")

    A suavização é feita por um envelope (juce::ADSR) para evitar estalos
    (clicks). Um corte instantâneo de volume cria uma descontinuidade na forma
    de onda, e o ouvido percebe isso como um "tic".

    FLUXO DO SINAL
    --------------

        Áudio In (estéreo) ──► [ x ganho ] ──► Áudio Out (estéreo)
                                   ▲
        MIDI In ──► ADSR ──► ganho = floor + (1 - floor) * envelope

    REGRAS DE OURO DA THREAD DE ÁUDIO (processBlock)
    ------------------------------------------------
    1. Nada de alocar memória (new, std::vector::push_back, juce::String...).
    2. Nada de locks (std::mutex, CriticalSection...).
    3. Nada de chamadas de sistema (arquivos, rede, console, DBG...).
    Tudo o que precisa de memória é criado no construtor ou em prepareToPlay().

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

//==============================================================================
/**
    Identificadores dos parâmetros.

    Ficam centralizados aqui para evitar erros de digitação: o mesmo texto é
    usado na criação do parâmetro e na leitura do valor. Nunca altere um ID
    depois de publicar o plugin, pois as sessões salvas na DAW usam esse texto
    para recuperar os valores.
*/
namespace ParamIDs
{
    inline constexpr const char* attack  = "attack";   // em milissegundos
    inline constexpr const char* release = "release";  // em milissegundos
    inline constexpr const char* floor   = "floor";    // em decibéis (dB)
}

//==============================================================================
class MidiGateAudioProcessor final : public juce::AudioProcessor
{
public:
    //==========================================================================
    MidiGateAudioProcessor();
    ~MidiGateAudioProcessor() override = default;

    //==========================================================================
    // Ciclo de vida do áudio

    /** Chamado pela DAW ANTES de o áudio começar a rodar (ou quando a taxa de
        amostragem/tamanho de bloco muda). É o lugar certo para preparar tudo
        que depende da sample rate. Pode alocar memória aqui. */
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;

    /** Chamado quando o áudio para. Libera recursos se necessário. */
    void releaseResources() override;

    /** Chamado quando a DAW quer "zerar" o estado (ex.: ao reposicionar a
        agulha de reprodução). Fecha o gate imediatamente. */
    void reset() override;

    /** Diz à DAW quais configurações de canais aceitamos. */
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    /** O coração do plugin: processa um bloco de áudio + MIDI.
        Roda na thread de áudio (tempo real). */
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock; // mantém a versão em double visível

    //==========================================================================
    // Interface gráfica
    //
    // Usamos o juce::GenericAudioProcessorEditor, que cria sliders
    // automaticamente para cada parâmetro. Assim o projeto não precisa de
    // PluginEditor.h/.cpp. Para criar uma interface própria depois, basta
    // criar uma classe de editor e retorná-la em createEditor().
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    // Informações para a DAW
    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override  { return true;  }  // precisamos do MIDI
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }  // processa áudio

    double getTailLengthSeconds() const override { return 0.0; }

    //==========================================================================
    // Programas (presets). Não usamos, mas a DAW exige pelo menos 1.
    int getNumPrograms() override                             { return 1; }
    int getCurrentProgram() override                          { return 0; }
    void setCurrentProgram (int) override                     {}
    const juce::String getProgramName (int) override          { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    // Salvar/carregar o estado (valores dos knobs) junto com o projeto da DAW
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    /** Árvore de parâmetros. Pública para que um editor customizado possa
        conectar sliders a ela (via SliderAttachment) no futuro. */
    juce::AudioProcessorValueTreeState apvts;

private:
    //==========================================================================
    /** Cria a lista de parâmetros expostos para a DAW (automação). */
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Lê os parâmetros e atualiza o ADSR e o floor. Chamado uma vez por
        bloco: barato e sem alocação. */
    void updateParameters();

    /** Trata uma única mensagem MIDI (Note On, Note Off, All Notes Off...). */
    void handleMidiMessage (const juce::MidiMessage& message);

    /** Aplica o ganho do gate às amostras [startSample, endSample). */
    void applyGate (juce::AudioBuffer<float>& buffer, int startSample, int endSample);

    //==========================================================================
    // Ponteiros "crus" para os valores dos parâmetros.
    //
    // O APVTS guarda cada valor num std::atomic<float>. Pegamos o ponteiro uma
    // única vez no construtor; depois, ler o valor na thread de áudio é só uma
    // leitura atômica (sem lock, sem busca por string, sem alocação).
    std::atomic<float>* attackParam  = nullptr;
    std::atomic<float>* releaseParam = nullptr;
    std::atomic<float>* floorParam   = nullptr;

    //==========================================================================
    // Envelope ADSR usado como "curva de abertura" do gate.
    //
    // Configuração usada:
    //   Attack  = parâmetro do usuário (tempo para ir de 0 até 1)
    //   Decay   = 0                    (não usamos)
    //   Sustain = 1.0                  (enquanto a nota está pressionada,
    //                                   o gate fica totalmente aberto)
    //   Release = parâmetro do usuário (tempo para voltar a 0)
    juce::ADSR adsr;
    juce::ADSR::Parameters adsrParams;

    // Últimos valores aplicados ao ADSR. Só chamamos adsr.setParameters()
    // quando algo mudou de fato, economizando CPU.
    float lastAttackMs  = -1.0f;
    float lastReleaseMs = -1.0f;

    //==========================================================================
    // Ganho linear do "Floor" (ex.: -20 dB -> 0.1). Suavizado para que mover o
    // knob (ou automatizar) durante a reprodução não gere estalos.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> floorGain;

    //==========================================================================
    // Controle de notas pressionadas.
    //
    // Se o usuário segura duas notas e solta só uma, o gate NÃO deve fechar.
    // Por isso guardamos quais notas estão pressionadas (16 canais x 128 notas)
    // e quantas são ao todo. O gate só fecha quando a contagem chega a zero.
    //
    // std::array tem tamanho fixo: a memória já existe dentro do objeto,
    // então usar isso na thread de áudio não aloca nada.
    static constexpr int numMidiChannels = 16;
    static constexpr int numMidiNotes    = 128;
    std::array<bool, numMidiChannels * numMidiNotes> noteIsHeld {};
    int numHeldNotes = 0;

    /** Esquece todas as notas e fecha o gate (com release suave). */
    void releaseAllNotes();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiGateAudioProcessor)
};

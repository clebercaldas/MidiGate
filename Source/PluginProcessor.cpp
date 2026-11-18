/*
  ==============================================================================

    MidiGate - PluginProcessor.cpp

    Implementação do gate acionado por MIDI.
    Leia primeiro os comentários de PluginProcessor.h para a visão geral.

  ==============================================================================
*/

#include "PluginProcessor.h"

//==============================================================================
// Limites dos parâmetros (centralizados para facilitar ajustes).
namespace
{
    // Attack mínimo de 0.1 ms: um attack igual a zero faria o ganho saltar
    // instantaneamente de "floor" para 1.0, o que causa estalo.
    constexpr float attackMinMs  = 0.1f;
    constexpr float attackMaxMs  = 500.0f;
    constexpr float attackDefMs  = 2.0f;

    // Release mínimo de 1 ms pelo mesmo motivo (fechamento suave).
    constexpr float releaseMinMs = 1.0f;
    constexpr float releaseMaxMs = 5000.0f;
    constexpr float releaseDefMs = 50.0f;

    // Floor: quanto o sinal é reduzido com o gate fechado.
    // -100 dB é tratado como silêncio absoluto (-inf).
    constexpr float floorMinDb   = -100.0f;
    constexpr float floorMaxDb   = 0.0f;
    constexpr float floorDefDb   = -100.0f;

    // Tempo de suavização do knob Floor (em segundos).
    constexpr double floorSmoothingSeconds = 0.02; // 20 ms
}

//==============================================================================
// CONSTRUTOR
//
// Aqui definimos os barramentos (entradas/saídas de áudio) e criamos o APVTS.
// Roda na thread principal (não é tempo real), então pode alocar memória.
//==============================================================================
MidiGateAudioProcessor::MidiGateAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this,                       // processador dono dos parâmetros
             nullptr,                     // sem UndoManager
             "MidiGateParameters",        // nome da árvore (usado no XML salvo)
             createParameterLayout())
{
    // Guardamos os ponteiros para os valores atômicos dos parâmetros.
    // Fazer isso aqui evita buscas por string dentro do processBlock.
    attackParam  = apvts.getRawParameterValue (ParamIDs::attack);
    releaseParam = apvts.getRawParameterValue (ParamIDs::release);
    floorParam   = apvts.getRawParameterValue (ParamIDs::floor);

    // Se algum ID estiver errado, o ponteiro volta nulo. Esta checagem só
    // existe em builds de Debug e ajuda a pegar o erro cedo.
    jassert (attackParam != nullptr && releaseParam != nullptr && floorParam != nullptr);
}

//==============================================================================
// CRIAÇÃO DOS PARÂMETROS
//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
MidiGateAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // --- Attack (ms) ---------------------------------------------------------
    // NormalisableRange(min, max, passo). O "skew" faz o knob ter mais
    // resolução nos valores pequenos, onde o ouvido é mais sensível.
    juce::NormalisableRange<float> attackRange (attackMinMs, attackMaxMs, 0.01f);
    attackRange.setSkewForCentre (20.0f); // o meio do knob fica em 20 ms

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::attack, 1 }, // ID + versão (exigido pelo AU)
        "Attack",                                  // nome exibido na DAW
        attackRange,
        attackDefMs,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    // --- Release (ms) --------------------------------------------------------
    juce::NormalisableRange<float> releaseRange (releaseMinMs, releaseMaxMs, 0.1f);
    releaseRange.setSkewForCentre (200.0f);

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::release, 1 },
        "Release",
        releaseRange,
        releaseDefMs,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    // --- Floor / Depth (dB) --------------------------------------------------
    // Mostramos "-inf" quando o knob está no mínimo, para deixar claro que é
    // silêncio total. Esta função de texto só roda na interface, nunca na
    // thread de áudio, então pode criar juce::String à vontade.
    auto floorToText = [] (float valueDb, int /*maxLength*/)
    {
        if (valueDb <= floorMinDb)
            return juce::String ("-inf");

        return juce::String (valueDb, 1);
    };

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::floor, 1 },
        "Floor",
        juce::NormalisableRange<float> (floorMinDb, floorMaxDb, 0.1f),
        floorDefDb,
        juce::AudioParameterFloatAttributes()
            .withLabel ("dB")
            .withStringFromValueFunction (floorToText)));

    return layout;
}

//==============================================================================
// PREPARAÇÃO (antes do áudio começar)
//==============================================================================
void MidiGateAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    // O ADSR precisa saber a sample rate para converter milissegundos em
    // número de amostras.
    adsr.setSampleRate (sampleRate);

    // Forçamos a reaplicação dos parâmetros do ADSR na primeira chamada de
    // updateParameters(), pois a sample rate pode ter mudado.
    lastAttackMs  = -1.0f;
    lastReleaseMs = -1.0f;

    // Configura a suavização do Floor e já posiciona no valor atual (sem
    // rampa inicial).
    floorGain.reset (sampleRate, floorSmoothingSeconds);
    floorGain.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (floorParam->load(), floorMinDb));

    updateParameters();
    reset();
}

void MidiGateAudioProcessor::releaseResources()
{
    // Nada a liberar: não alocamos nenhum recurso dinâmico.
}

void MidiGateAudioProcessor::reset()
{
    // Fecha o gate imediatamente e esquece as notas pressionadas.
    adsr.reset();
    noteIsHeld.fill (false);
    numHeldNotes = 0;
}

//==============================================================================
// LAYOUT DE CANAIS
//==============================================================================
bool MidiGateAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainOut = layouts.getMainOutputChannelSet();
    const auto& mainIn  = layouts.getMainInputChannelSet();

    // O plugin foi pensado para estéreo. Aceitamos também mono -> mono, pois
    // a lógica é idêntica e isso evita que a DAW recuse o plugin em trilhas
    // mono. A entrada sempre precisa ser igual à saída.
    const bool isStereoOrMono = (mainOut == juce::AudioChannelSet::stereo()
                              || mainOut == juce::AudioChannelSet::mono());

    return isStereoOrMono && mainIn == mainOut;
}

//==============================================================================
// LEITURA DOS PARÂMETROS (uma vez por bloco)
//==============================================================================
void MidiGateAudioProcessor::updateParameters()
{
    // load() é uma leitura atômica: segura entre threads e sem lock.
    const float attackMs  = attackParam->load();
    const float releaseMs = releaseParam->load();
    const float floorDb   = floorParam->load();

    // Só reconfigura o ADSR se attack/release mudaram.
    // juce::exactlyEqual deixa explícito que queremos comparação exata
    // (detectar "o valor mudou?"), e não uma comparação aproximada.
    if (! juce::exactlyEqual (attackMs, lastAttackMs)
        || ! juce::exactlyEqual (releaseMs, lastReleaseMs))
    {
        adsrParams.attack  = attackMs  * 0.001f; // ms -> segundos
        adsrParams.decay   = 0.0f;
        adsrParams.sustain = 1.0f;
        adsrParams.release = releaseMs * 0.001f; // ms -> segundos

        adsr.setParameters (adsrParams); // apenas cálculos, sem alocação

        lastAttackMs  = attackMs;
        lastReleaseMs = releaseMs;
    }

    // Converte dB para ganho linear.
    //   0 dB   -> 1.0   (gate fechado não reduz nada)
    //  -6 dB   -> ~0.5
    //  -20 dB  -> 0.1
    //  -100 dB -> 0.0   (tratado como silêncio absoluto)
    floorGain.setTargetValue (juce::Decibels::decibelsToGain (floorDb, floorMinDb));
}

//==============================================================================
// TRATAMENTO DE MIDI
//==============================================================================
void MidiGateAudioProcessor::handleMidiMessage (const juce::MidiMessage& message)
{
    if (message.isNoteOn())
    {
        // getChannel() retorna 1..16; convertemos para 0..15.
        const int index = (message.getChannel() - 1) * numMidiNotes + message.getNoteNumber();

        if (! noteIsHeld[(size_t) index])
        {
            noteIsHeld[(size_t) index] = true;
            ++numHeldNotes;

            // Primeira nota pressionada: abre o gate.
            // Notas extras enquanto o gate já está aberto não fazem nada
            // (comportamento "legato"), evitando reinícios desnecessários.
            if (numHeldNotes == 1)
                adsr.noteOn();
        }
    }
    else if (message.isNoteOff()) // inclui "Note On com velocity 0"
    {
        const int index = (message.getChannel() - 1) * numMidiNotes + message.getNoteNumber();

        if (noteIsHeld[(size_t) index])
        {
            noteIsHeld[(size_t) index] = false;
            --numHeldNotes;

            // Última nota solta: fecha o gate (fase de release).
            if (numHeldNotes == 0)
                adsr.noteOff();
        }
    }
    else if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        // Mensagens de "pânico" (CC 123 / CC 120): fecha o gate.
        releaseAllNotes();
    }
}

void MidiGateAudioProcessor::releaseAllNotes()
{
    noteIsHeld.fill (false);
    numHeldNotes = 0;
    adsr.noteOff(); // fecha com release suave (não corta seco)
}

//==============================================================================
// APLICAÇÃO DO GANHO EM UM TRECHO DO BLOCO
//==============================================================================
void MidiGateAudioProcessor::applyGate (juce::AudioBuffer<float>& buffer,
                                        int startSample, int endSample)
{
    const int numSamples  = endSample - startSample;
    const int numChannels = buffer.getNumChannels();

    if (numSamples <= 0)
        return;

    // ATALHO DE CPU: gate totalmente fechado e floor estável.
    // Neste caso o ganho é constante no trecho todo, então não é preciso
    // calcular amostra por amostra.
    if (! adsr.isActive() && ! floorGain.isSmoothing())
    {
        const float g = floorGain.getTargetValue();

        if (g <= 0.0f)
            buffer.clear (startSample, numSamples);          // silêncio
        else if (g < 1.0f)
            buffer.applyGain (startSample, numSamples, g);   // atenuação fixa
        // g == 1.0 (floor em 0 dB): nada a fazer, o áudio passa intacto.

        return;
    }

    // Caminho normal: o envelope está em movimento (ou sustentando).
    // Pegamos os ponteiros de escrita dos canais uma vez só, fora do loop.
    float* left  = buffer.getWritePointer (0);
    float* right = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int i = startSample; i < endSample; ++i)
    {
        // Valor do envelope nesta amostra: 0.0 (fechado) .. 1.0 (aberto).
        const float env = adsr.getNextSample();

        // Floor suavizado nesta amostra.
        const float fl = floorGain.getNextValue();

        // Interpolação linear entre o floor e o ganho total:
        //   env = 0 -> ganho = floor
        //   env = 1 -> ganho = 1.0
        const float gain = fl + (1.0f - fl) * env;

        left[i] *= gain;

        if (right != nullptr)
            right[i] *= gain;
    }
}

//==============================================================================
// PROCESSAMENTO DE ÁUDIO (THREAD DE TEMPO REAL)
//==============================================================================
void MidiGateAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                           juce::MidiBuffer& midiMessages)
{
    // Evita "denormals": números minúsculos (ex.: cauda do release chegando a
    // zero) que deixam a CPU muito lenta em alguns processadores.
    juce::ScopedNoDenormals noDenormals;

    // Se a DAW nos der mais canais de saída do que de entrada, limpamos os
    // extras para não deixar lixo de memória sair como ruído.
    const int numInputs  = getTotalNumInputChannels();
    const int numOutputs = getTotalNumOutputChannels();
    for (int ch = numInputs; ch < numOutputs; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // 1) Lê os knobs uma vez por bloco.
    updateParameters();

    // 2) Processamento com precisão de amostra (sample-accurate).
    //
    // As mensagens MIDI chegam com uma posição dentro do bloco
    // (samplePosition). Em vez de abrir o gate no início do bloco, dividimos
    // o bloco em pedaços:
    //
    //   |----- trecho A -----|N|------ trecho B ------|
    //   0                   pos                      fim
    //
    // Processamos o trecho A com o estado antigo, aplicamos a nota N e então
    // processamos o trecho B. Assim o gate abre exatamente no ponto certo,
    // independentemente do tamanho do buffer configurado na DAW.
    //
    // Iterar o MidiBuffer com "for (const auto metadata : ...)" não aloca
    // memória: apenas lê os bytes já armazenados.
    const int numSamples = buffer.getNumSamples();
    int currentSample = 0;

    for (const auto metadata : midiMessages)
    {
        // Garante que a posição está dentro do bloco (proteção contra hosts
        // que enviam posições fora do intervalo).
        const int eventPos = juce::jlimit (0, numSamples, metadata.samplePosition);

        applyGate (buffer, currentSample, eventPos); // áudio até o evento
        handleMidiMessage (metadata.getMessage());   // muda o estado do gate
        currentSample = eventPos;
    }

    // 3) Processa o restante do bloco após o último evento MIDI.
    applyGate (buffer, currentSample, numSamples);

    // Observação: não limpamos o midiMessages. As mensagens seguem adiante
    // na cadeia caso a DAW as repasse, o que não interfere no áudio.
}

//==============================================================================
// EDITOR
//==============================================================================
juce::AudioProcessorEditor* MidiGateAudioProcessor::createEditor()
{
    // Interface automática: um slider por parâmetro.
    return new juce::GenericAudioProcessorEditor (*this);
}

//==============================================================================
// SALVAR / CARREGAR ESTADO
//
// Rodam na thread de mensagens (não é tempo real), então XML e alocações
// são permitidos aqui.
//==============================================================================
void MidiGateAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();

    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MidiGateAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (const auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
// PONTO DE ENTRADA
//
// O JUCE chama esta função para criar uma nova instância do plugin
// sempre que o usuário o insere numa trilha.
//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MidiGateAudioProcessor();
}

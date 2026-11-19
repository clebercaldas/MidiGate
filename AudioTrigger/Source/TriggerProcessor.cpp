/*
  ==============================================================================

    AudioTrigger - TriggerProcessor.cpp

    Leia primeiro os comentários de TriggerProcessor.h.

  ==============================================================================
*/

#include "TriggerProcessor.h"
#include <cmath>

//==============================================================================
namespace
{
    // Faixas e valores padrão dos parâmetros.
    constexpr float thresholdMinDb = -60.0f, thresholdMaxDb = -1.0f,  thresholdDefDb = -30.0f;
    constexpr float hysteresisMaxDb = 24.0f,                          hysteresisDefDb = 6.0f;
    constexpr float releaseMinMs = 1.0f, releaseMaxMs = 500.0f,       releaseDefMs = 30.0f;
    constexpr float lockoutMaxMs = 500.0f,                            lockoutDefMs = 20.0f;
    constexpr float velWindowMaxMs = 10.0f,                           velWindowDefMs = 2.0f;
    constexpr float velLowMinDb = -60.0f, velLowMaxDb = -1.0f,        velLowDefDb = -30.0f;
    constexpr float velHighMinDb = -30.0f, velHighMaxDb = 0.0f,       velHighDefDb = -3.0f;
    constexpr float velCurveMin = 0.3f, velCurveMax = 3.0f,           velCurveDef = 1.0f;
    constexpr int   noteDef = 36;      // C1: o bumbo na maioria dos kits GM
    constexpr int   firstVirtualPortId = 1;

    // Contador para dar nomes diferentes às portas virtuais quando existe mais
    // de uma instância do plugin na sessão.
    std::atomic<int> portInstanceCounter { firstVirtualPortId };
}

//==============================================================================
// PORTA MIDI VIRTUAL
//==============================================================================
VirtualMidiPort::VirtualMidiPort (const juce::String& portName)
    : juce::Thread ("AudioTrigger MIDI out")
{
    // createNewDevice cria uma "fonte MIDI" que outros programas (o Logic)
    // enxergam como uma entrada MIDI. Só existe no macOS e no Linux.
    output = juce::MidiOutput::createNewDevice (portName);

    if (output != nullptr)
        startThread();
}

VirtualMidiPort::~VirtualMidiPort()
{
    stopThread (500); // a thread ainda esvazia a fila uma última vez ao sair
}

void VirtualMidiPort::push (juce::uint8 status, juce::uint8 data1, juce::uint8 data2) noexcept
{
    if (output == nullptr)
        return;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 > 0)
    {
        auto& e = events[(size_t) start1];
        e.bytes[0] = status;
        e.bytes[1] = data1;
        e.bytes[2] = data2;
        fifo.finishedWrite (1);
    }
    // Se a fila estiver cheia (impossível na prática: 256 eventos), o evento
    // é descartado em vez de bloquear a thread de áudio.
}

void VirtualMidiPort::run()
{
    while (! threadShouldExit())
    {
        drain();
        wait (1); // dorme ~1 ms; esta thread NÃO é a de áudio
    }

    drain();
}

void VirtualMidiPort::drain()
{
    while (fifo.getNumReady() > 0)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (1, start1, size1, start2, size2);

        if (size1 <= 0)
            break;

        const auto e = events[(size_t) start1];
        fifo.finishedRead (1);

        output->sendMessageNow (juce::MidiMessage ((int) e.bytes[0], (int) e.bytes[1], (int) e.bytes[2]));
    }
}

//==============================================================================
// CONSTRUTOR / DESTRUTOR
//==============================================================================
TriggerAudioProcessor::TriggerAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "AudioTriggerParameters", createParameterLayout())
{
    thresholdParam  = apvts.getRawParameterValue (TriggerParamIDs::threshold);
    hysteresisParam = apvts.getRawParameterValue (TriggerParamIDs::hysteresis);
    releaseParam    = apvts.getRawParameterValue (TriggerParamIDs::release);
    lockoutParam    = apvts.getRawParameterValue (TriggerParamIDs::lockout);
    velWindowParam  = apvts.getRawParameterValue (TriggerParamIDs::velWindow);
    noteParam       = apvts.getRawParameterValue (TriggerParamIDs::note);
    channelParam    = apvts.getRawParameterValue (TriggerParamIDs::channel);
    velocityParam   = apvts.getRawParameterValue (TriggerParamIDs::velocity);
    velLowParam     = apvts.getRawParameterValue (TriggerParamIDs::velLow);
    velHighParam    = apvts.getRawParameterValue (TriggerParamIDs::velHigh);
    velCurveParam   = apvts.getRawParameterValue (TriggerParamIDs::velCurve);
    portParam       = apvts.getRawParameterValue (TriggerParamIDs::port);

    jassert (thresholdParam && hysteresisParam && releaseParam && lockoutParam && velWindowParam
             && noteParam && channelParam && velocityParam && velLowParam && velHighParam
             && velCurveParam && portParam);

    const int n = portInstanceCounter.fetch_add (1);
    const juce::String portName = n == firstVirtualPortId ? juce::String ("AudioTrigger Out")
                                                           : "AudioTrigger Out " + juce::String (n);
    virtualPort = std::make_unique<VirtualMidiPort> (portName);
}

TriggerAudioProcessor::~TriggerAudioProcessor()
{
    // Não deixa nota presa no instrumento ao remover o plugin.
    if (virtualPort != nullptr && heldNote >= 0)
        virtualPort->push ((juce::uint8) (0x80 | (heldChannel - 1)), (juce::uint8) heldNote, 0);
}

//==============================================================================
// PARÂMETROS
//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
TriggerAudioProcessor::createParameterLayout()
{
    using FloatParam = juce::AudioParameterFloat;
    using IntParam   = juce::AudioParameterInt;
    using BoolParam  = juce::AudioParameterBool;
    using Attr       = juce::AudioParameterFloatAttributes;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { TriggerParamIDs::threshold, 1 }, "Threshold",
        juce::NormalisableRange<float> (thresholdMinDb, thresholdMaxDb, 0.1f),
        thresholdDefDb, Attr().withLabel ("dB")));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { TriggerParamIDs::hysteresis, 1 }, "Hysteresis",
        juce::NormalisableRange<float> (0.0f, hysteresisMaxDb, 0.1f),
        hysteresisDefDb, Attr().withLabel ("dB")));

    juce::NormalisableRange<float> releaseRange (releaseMinMs, releaseMaxMs, 0.1f);
    releaseRange.setSkewForCentre (50.0f);
    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { TriggerParamIDs::release, 1 }, "Release",
        releaseRange, releaseDefMs, Attr().withLabel ("ms")));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { TriggerParamIDs::lockout, 1 }, "Retrigger Lockout",
        juce::NormalisableRange<float> (0.0f, lockoutMaxMs, 1.0f),
        lockoutDefMs, Attr().withLabel ("ms")));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { TriggerParamIDs::velWindow, 1 }, "Velocity Window",
        juce::NormalisableRange<float> (0.0f, velWindowMaxMs, 0.1f),
        velWindowDefMs, Attr().withLabel ("ms")));

    layout.add (std::make_unique<IntParam> (
        juce::ParameterID { TriggerParamIDs::note, 1 }, "Note", 0, 127, noteDef));

    layout.add (std::make_unique<IntParam> (
        juce::ParameterID { TriggerParamIDs::channel, 1 }, "MIDI Channel", 1, 16, 1));

    // 0 = a velocity vem da amplitude do golpe. 1..127 = valor fixo.
    layout.add (std::make_unique<IntParam> (
        juce::ParameterID { TriggerParamIDs::velocity, 1 }, "Fixed Velocity (0 = from amplitude)", 0, 127, 0));

    // Conversão amplitude -> velocity: o pico em "Velocity Low" vira 1 e o pico
    // em "Velocity High" vira 127. Entre os dois a velocity cresce em dB.
    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { TriggerParamIDs::velLow, 1 }, "Velocity Low (peak -> 1)",
        juce::NormalisableRange<float> (velLowMinDb, velLowMaxDb, 0.1f),
        velLowDefDb, Attr().withLabel ("dB")));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { TriggerParamIDs::velHigh, 1 }, "Velocity High (peak -> 127)",
        juce::NormalisableRange<float> (velHighMinDb, velHighMaxDb, 0.1f),
        velHighDefDb, Attr().withLabel ("dB")));

    // Curva: 1 = linear. Menor que 1 deixa golpes fracos mais fortes;
    // maior que 1 exige golpes mais fortes para chegar à velocity máxima.
    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { TriggerParamIDs::velCurve, 1 }, "Velocity Curve",
        juce::NormalisableRange<float> (velCurveMin, velCurveMax, 0.01f),
        velCurveDef));

    layout.add (std::make_unique<BoolParam> (
        juce::ParameterID { TriggerParamIDs::port, 1 }, "Send to Virtual MIDI Port", true));

    return layout;
}

//==============================================================================
// PREPARAÇÃO
//==============================================================================
void TriggerAudioProcessor::prepareToPlay (double newSampleRate, int /*samplesPerBlock*/)
{
    sampleRate = newSampleRate;
    reset();
}

void TriggerAudioProcessor::reset()
{
    // Pode ser chamado por outra thread: só levanta uma bandeira. O
    // processBlock fecha a nota presa e zera o detector.
    pendingReset.store (true);
}

bool TriggerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono())
           && layouts.getMainInputChannelSet() == out;
}

//==============================================================================
// PROCESSAMENTO (THREAD DE ÁUDIO)
//==============================================================================
void TriggerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Este plugin não recebe MIDI; começamos o bloco com a lista vazia e só
    // adicionamos os eventos gerados aqui.
    midiMessages.clear();

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    const float* const* in = buffer.getArrayOfReadPointers();

    //--------------------------------------------------------------------------
    // 1) Lê os parâmetros UMA vez por bloco e converte para valores de trabalho.
    const float thresholdDb = thresholdParam->load();
    const float openLevel   = juce::Decibels::decibelsToGain (thresholdDb);
    const float closeLevel  = juce::Decibels::decibelsToGain (thresholdDb - hysteresisParam->load());

    // Seguidor de pico: sobe na hora, desce com uma curva exponencial. O
    // coeficiente abaixo faz o valor cair ~63% a cada "release" ms.
    const float releaseSeconds = releaseParam->load() * 0.001f;
    const float releaseCoeff   = std::exp (-1.0f / (float) (sampleRate * releaseSeconds));

    const int lockoutSamples = (int) std::lround (sampleRate * lockoutParam->load() * 0.001);
    const int fixedVelocity  = (int) std::lround (velocityParam->load());
    const int windowSamples  = fixedVelocity > 0
                                 ? 0
                                 : (int) std::lround (sampleRate * velWindowParam->load() * 0.001);

    const int note    = juce::jlimit (0, 127, (int) std::lround (noteParam->load()));
    const int channel = juce::jlimit (1, 16,  (int) std::lround (channelParam->load()));
    const bool usePort = virtualPort != nullptr && portParam->load() >= 0.5f;

    //--------------------------------------------------------------------------
    // Funções auxiliares: mandam o evento para os DOIS caminhos de saída.
    auto sendNoteOn = [&] (int samplePos, int velocity)
    {
        midiMessages.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) velocity), samplePos);

        if (usePort)
            virtualPort->push ((juce::uint8) (0x90 | (channel - 1)), (juce::uint8) note, (juce::uint8) velocity);

        heldNote    = note;
        heldChannel = channel;
        state       = State::open;
    };

    auto sendNoteOff = [&] (int samplePos)
    {
        // Usa a nota e o canal do Note On, mesmo que o usuário tenha mexido
        // nos parâmetros enquanto a nota soava.
        midiMessages.addEvent (juce::MidiMessage::noteOff (heldChannel, heldNote), samplePos);

        if (virtualPort != nullptr)
            virtualPort->push ((juce::uint8) (0x80 | (heldChannel - 1)), (juce::uint8) heldNote, 0);

        heldNote = -1;
    };

    // Converte o pico medido (amplitude) em velocity MIDI:
    //   pico <= Velocity Low  -> 1
    //   pico >= Velocity High -> 127
    //   entre os dois         -> cresce linearmente em dB, moldado pela curva.
    const float velLowDb  = velLowParam->load();
    const float velHighDb = juce::jmax (velHighParam->load(), velLowDb + 1.0f);
    const float velCurve  = velCurveParam->load();

    auto velocityFromPeak = [&] (float peak)
    {
        const float peakDb = juce::Decibels::gainToDecibels (peak, -100.0f);
        float t = juce::jlimit (0.0f, 1.0f, (peakDb - velLowDb) / (velHighDb - velLowDb));
        t = std::pow (t, velCurve); // roda uma vez por golpe, não por amostra
        return juce::jlimit (1, 127, 1 + (int) std::lround (126.0f * t));
    };

    //--------------------------------------------------------------------------
    // 2) Se houve reset (transporte parou, etc.), fecha a nota que estiver presa.
    if (pendingReset.exchange (false))
    {
        if (state == State::open && heldNote >= 0)
            sendNoteOff (0);

        state       = State::idle;
        envelope    = 0.0f;
        lockoutLeft = 0;
        windowLeft  = 0;
    }

    //--------------------------------------------------------------------------
    // 3) Processa amostra por amostra: a precisão do disparo é de 1 amostra.
    for (int i = 0; i < numSamples; ++i)
    {
        // Pico entre os canais nesta amostra.
        float x = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            x = juce::jmax (x, std::abs (in[ch][i]));

        // Seguidor de pico: sobe na hora, desce suavemente.
        envelope = x > envelope ? x : envelope * releaseCoeff;

        if (lockoutLeft > 0)
            --lockoutLeft;

        switch (state)
        {
            case State::idle:
                if (envelope >= openLevel && lockoutLeft == 0)
                {
                    if (fixedVelocity > 0)
                    {
                        sendNoteOn (i, fixedVelocity);   // dispara imediatamente
                    }
                    else if (windowSamples <= 0)
                    {
                        sendNoteOn (i, velocityFromPeak (envelope));
                    }
                    else
                    {
                        // Espera alguns ms medindo o pico, para a velocity
                        // refletir a força real do golpe.
                        state      = State::measuring;
                        windowPeak = envelope;
                        windowLeft = windowSamples;
                    }
                }
                break;

            case State::measuring:
                windowPeak = juce::jmax (windowPeak, envelope);

                if (--windowLeft <= 0)
                    sendNoteOn (i, velocityFromPeak (windowPeak));
                break;

            case State::open:
                if (envelope < closeLevel)
                {
                    sendNoteOff (i);
                    state       = State::idle;
                    lockoutLeft = lockoutSamples;
                }
                break;
        }
    }

    // O áudio não foi modificado: passa direto para a saída.
}

//==============================================================================
juce::AudioProcessorEditor* TriggerAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void TriggerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto tree = apvts.copyState();

    if (const auto xml = tree.createXml())
        copyXmlToBinary (*xml, destData);
}

void TriggerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (const auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TriggerAudioProcessor();
}

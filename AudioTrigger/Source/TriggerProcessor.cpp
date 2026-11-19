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
    // Faixas e valores padrão dos parâmetros (os do envelope seguem o ReaGate).
    constexpr float thresholdMinDb = -80.0f, thresholdMaxDb = 0.0f,    thresholdDefDb = -30.0f;
    constexpr float hysteresisMaxDb = 24.0f,                           hysteresisDefDb = 6.0f;
    constexpr float preOpenMaxMs = 100.0f;
    constexpr float attackMaxMs = 250.0f,                              attackDefMs = 3.0f;
    constexpr float holdMaxMs = 1000.0f;
    constexpr float releaseMaxMs = 2000.0f,                            releaseDefMs = 100.0f;
    constexpr float lowpassMinHz = 20.0f, lowpassMaxHz = 20000.0f;
    constexpr float highpassMaxHz = 5000.0f;
    constexpr float rmsMaxMs = 100.0f;
    constexpr float mixMinDb = -120.0f, mixMaxDb = 12.0f;
    constexpr float lockoutMaxMs = 500.0f,                             lockoutDefMs = 20.0f;
    constexpr float velWindowMaxMs = 10.0f,                            velWindowDefMs = 2.0f;
    constexpr float velLowMinDb = -60.0f, velLowMaxDb = -1.0f,         velLowDefDb = -30.0f;
    constexpr float velHighMinDb = -30.0f, velHighMaxDb = 0.0f,        velHighDefDb = -3.0f;
    constexpr float velCurveMin = 0.3f, velCurveMax = 3.0f,            velCurveDef = 1.0f;
    constexpr int   noteDef = 36;      // C1: o bumbo na maioria dos kits GM
    constexpr int   firstVirtualPortId = 1;

    // O detector de pico decai com esta constante de tempo. Ela evita que o
    // gate "tremule" nos cruzamentos por zero da forma de onda.
    constexpr double peakDecaySeconds = 0.010;

    // Filtros do detector: "desligado" quando o knob está nos extremos.
    constexpr float lowpassOffHz  = 19999.0f;
    constexpr float highpassOffHz = 20.0f;

    // Ponto em dB abaixo do qual os knobs de mix valem "menos infinito".
    constexpr float mixMinusInfDb = -119.0f;

    // Contador para dar nomes diferentes às portas virtuais quando existe mais
    // de uma instância do plugin na sessão.
    std::atomic<int> portInstanceCounter { firstVirtualPortId };

    juce::String dbToText (float valueDb, int)
    {
        return valueDb <= mixMinusInfDb ? juce::String ("-inf") : juce::String (valueDb, 1);
    }
}

//==============================================================================
// FILTROS (coeficientes RBJ, Butterworth Q = 0.7071)
//==============================================================================
void TriggerAudioProcessor::Biquad::setLowpass (double sr, double frequency)
{
    const double f     = juce::jlimit (10.0, sr * 0.45, frequency);
    const double w0    = juce::MathConstants<double>::twoPi * f / sr;
    const double cosw  = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * 0.70710678);
    const double a0    = 1.0 + alpha;

    b0 = (float) (((1.0 - cosw) * 0.5) / a0);
    b1 = (float) ((1.0 - cosw) / a0);
    b2 = b0;
    a1 = (float) ((-2.0 * cosw) / a0);
    a2 = (float) ((1.0 - alpha) / a0);
}

void TriggerAudioProcessor::Biquad::setHighpass (double sr, double frequency)
{
    const double f     = juce::jlimit (10.0, sr * 0.45, frequency);
    const double w0    = juce::MathConstants<double>::twoPi * f / sr;
    const double cosw  = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * 0.70710678);
    const double a0    = 1.0 + alpha;

    b0 = (float) (((1.0 + cosw) * 0.5) / a0);
    b1 = (float) (-(1.0 + cosw) / a0);
    b2 = b0;
    a1 = (float) ((-2.0 * cosw) / a0);
    a2 = (float) ((1.0 - alpha) / a0);
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
    namespace P = TriggerParamIDs;

    thresholdParam  = apvts.getRawParameterValue (P::threshold);
    hysteresisParam = apvts.getRawParameterValue (P::hysteresis);
    preOpenParam    = apvts.getRawParameterValue (P::preOpen);
    attackParam     = apvts.getRawParameterValue (P::attack);
    holdParam       = apvts.getRawParameterValue (P::hold);
    releaseParam    = apvts.getRawParameterValue (P::release);
    lowpassParam    = apvts.getRawParameterValue (P::lowpass);
    highpassParam   = apvts.getRawParameterValue (P::highpass);
    rmsParam        = apvts.getRawParameterValue (P::rmsSize);
    previewParam    = apvts.getRawParameterValue (P::preview);
    wetParam        = apvts.getRawParameterValue (P::wet);
    dryParam        = apvts.getRawParameterValue (P::dry);
    noiseParam      = apvts.getRawParameterValue (P::noise);
    sendMidiParam   = apvts.getRawParameterValue (P::sendMidi);
    noteParam       = apvts.getRawParameterValue (P::note);
    channelParam    = apvts.getRawParameterValue (P::channel);
    velocityParam   = apvts.getRawParameterValue (P::velocity);
    velLowParam     = apvts.getRawParameterValue (P::velLow);
    velHighParam    = apvts.getRawParameterValue (P::velHigh);
    velCurveParam   = apvts.getRawParameterValue (P::velCurve);
    velWindowParam  = apvts.getRawParameterValue (P::velWindow);
    lockoutParam    = apvts.getRawParameterValue (P::lockout);
    portParam       = apvts.getRawParameterValue (P::port);

    jassert (thresholdParam && hysteresisParam && preOpenParam && attackParam && holdParam
             && releaseParam && lowpassParam && highpassParam && rmsParam && previewParam
             && wetParam && dryParam && noiseParam && sendMidiParam && noteParam
             && channelParam && velocityParam && velLowParam && velHighParam && velCurveParam
             && velWindowParam && lockoutParam && portParam);

    const int n = portInstanceCounter.fetch_add (1);
    const juce::String portName = n == firstVirtualPortId ? juce::String ("AudioTrigger Out")
                                                           : "AudioTrigger Out " + juce::String (n);
    virtualPort = std::make_unique<VirtualMidiPort> (portName);

    startTimer (200); // confere o Pre-open e informa a latência à DAW
}

TriggerAudioProcessor::~TriggerAudioProcessor()
{
    stopTimer();

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
    namespace P = TriggerParamIDs;
    using FloatParam = juce::AudioParameterFloat;
    using IntParam   = juce::AudioParameterInt;
    using BoolParam  = juce::AudioParameterBool;
    using Attr       = juce::AudioParameterFloatAttributes;
    using Range      = juce::NormalisableRange<float>;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto addFloat = [&layout] (const char* id, const char* name, Range range, float def, const char* label)
    {
        layout.add (std::make_unique<FloatParam> (juce::ParameterID { id, 1 }, name, range, def,
                                                  Attr().withLabel (label)));
    };

    // --- Detector e envelope --------------------------------------------------
    addFloat (P::threshold,  "Threshold",  Range (thresholdMinDb, thresholdMaxDb, 0.1f), thresholdDefDb, "dB");
    addFloat (P::hysteresis, "Hysteresis", Range (0.0f, hysteresisMaxDb, 0.1f),         hysteresisDefDb, "dB");
    addFloat (P::preOpen,    "Pre-open",   Range (0.0f, preOpenMaxMs, 0.1f),            0.0f, "ms");

    Range attackRange (0.0f, attackMaxMs, 0.1f);
    attackRange.setSkewForCentre (20.0f);
    addFloat (P::attack, "Attack", attackRange, attackDefMs, "ms");

    Range holdRange (0.0f, holdMaxMs, 1.0f);
    holdRange.setSkewForCentre (100.0f);
    addFloat (P::hold, "Hold", holdRange, 0.0f, "ms");

    Range releaseRange (0.0f, releaseMaxMs, 1.0f);
    releaseRange.setSkewForCentre (200.0f);
    addFloat (P::release, "Release", releaseRange, releaseDefMs, "ms");

    Range lowpassRange (lowpassMinHz, lowpassMaxHz, 1.0f);
    lowpassRange.setSkewForCentre (1000.0f);
    addFloat (P::lowpass, "Lowpass", lowpassRange, lowpassMaxHz, "Hz");

    Range highpassRange (0.0f, highpassMaxHz, 1.0f);
    highpassRange.setSkewForCentre (300.0f);
    addFloat (P::highpass, "Highpass", highpassRange, 0.0f, "Hz");

    addFloat (P::rmsSize, "RMS size (0 = peak)", Range (0.0f, rmsMaxMs, 0.1f), 0.0f, "ms");

    layout.add (std::make_unique<BoolParam> (juce::ParameterID { P::preview, 1 },
                                             "Preview filter output", false));

    // --- Mix de saída -----------------------------------------------------------
    auto addMix = [&layout] (const char* id, const char* name, float def)
    {
        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { id, 1 }, name, Range (mixMinDb, mixMaxDb, 0.1f), def,
            Attr().withLabel ("dB").withStringFromValueFunction (dbToText)));
    };

    addMix (P::wet,   "Wet",   0.0f);
    addMix (P::dry,   "Dry",   mixMinDb);
    addMix (P::noise, "Noise", mixMinDb);

    // --- MIDI ---------------------------------------------------------------------
    layout.add (std::make_unique<BoolParam> (juce::ParameterID { P::sendMidi, 1 },
                                             "Send MIDI on open/close", true));

    layout.add (std::make_unique<IntParam> (juce::ParameterID { P::note, 1 }, "Note", 0, 127, noteDef));
    layout.add (std::make_unique<IntParam> (juce::ParameterID { P::channel, 1 }, "MIDI Channel", 1, 16, 1));

    // 0 = a velocity vem da amplitude do golpe. 1..127 = valor fixo.
    layout.add (std::make_unique<IntParam> (juce::ParameterID { P::velocity, 1 },
                                            "Fixed Velocity (0 = from amplitude)", 0, 127, 0));

    // Conversão amplitude -> velocity: o pico em "Velocity Low" vira 1 e o pico
    // em "Velocity High" vira 127. Entre os dois a velocity cresce em dB.
    addFloat (P::velLow,  "Velocity Low (peak -> 1)",   Range (velLowMinDb, velLowMaxDb, 0.1f),   velLowDefDb,  "dB");
    addFloat (P::velHigh, "Velocity High (peak -> 127)", Range (velHighMinDb, velHighMaxDb, 0.1f), velHighDefDb, "dB");

    // Curva: 1 = linear. Menor que 1 deixa golpes fracos mais fortes; maior
    // que 1 exige golpes mais fortes para chegar à velocity máxima.
    layout.add (std::make_unique<FloatParam> (juce::ParameterID { P::velCurve, 1 }, "Velocity Curve",
                                              Range (velCurveMin, velCurveMax, 0.01f), velCurveDef));

    addFloat (P::velWindow, "Velocity Window", Range (0.0f, velWindowMaxMs, 0.1f), velWindowDefMs, "ms");
    addFloat (P::lockout,   "Retrigger Lockout", Range (0.0f, lockoutMaxMs, 1.0f),   lockoutDefMs,   "ms");

    layout.add (std::make_unique<BoolParam> (juce::ParameterID { P::port, 1 },
                                             "Send to Virtual MIDI Port", true));

    return layout;
}

//==============================================================================
// PREPARAÇÃO
//==============================================================================
void TriggerAudioProcessor::prepareToPlay (double newSampleRate, int /*samplesPerBlock*/)
{
    sampleRate = newSampleRate;

    // Todos os buffers são criados AQUI, com o tamanho máximo, para o
    // processBlock nunca precisar alocar memória.
    const auto maxRmsSamples   = (size_t) std::ceil (sampleRate * rmsMaxMs * 0.001) + 2;
    const auto maxDelaySamples = (size_t) std::ceil (sampleRate * preOpenMaxMs * 0.001) + 2;

    rmsBuffer.assign (maxRmsSamples, 0.0f);
    for (auto& d : delayBuffer)
        d.assign (maxDelaySamples, 0.0f);

    wetGain.reset (sampleRate, 0.02);
    dryGain.reset (sampleRate, 0.02);
    noiseGain.reset (sampleRate, 0.02);
    wetGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (wetParam->load(), mixMinusInfDb));
    dryGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (dryParam->load(), mixMinusInfDb));
    noiseGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (noiseParam->load(), mixMinusInfDb));

    lastLowpassHz = lastHighpassHz = -1.0f;
    lastRmsLength = -1;

    pendingReset.store (true);
    timerCallback();
}

void TriggerAudioProcessor::reset()
{
    // Pode ser chamado por outra thread: só levanta uma bandeira. O
    // processBlock fecha a nota presa e zera o detector.
    pendingReset.store (true);
}

void TriggerAudioProcessor::timerCallback()
{
    // O Pre-open atrasa o áudio. A DAW precisa saber, para alinhar as trilhas.
    const int latency = (int) std::lround (sampleRate * preOpenParam->load() * 0.001);

    if (latency != getLatencySamples())
        setLatencySamples (latency);
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
    const int numChannels = juce::jmin (buffer.getNumChannels(), 2, getTotalNumInputChannels());
    float* const* data    = buffer.getArrayOfWritePointers();

    //--------------------------------------------------------------------------
    // 1) Lê os parâmetros UMA vez por bloco e converte para valores de trabalho.
    const float thresholdDb = thresholdParam->load();
    const float openLevel   = juce::Decibels::decibelsToGain (thresholdDb);
    const float closeLevel  = juce::Decibels::decibelsToGain (thresholdDb - hysteresisParam->load());

    const float attackStep  = 1.0f / (float) juce::jmax (1.0, sampleRate * attackParam->load() * 0.001);
    const float releaseStep = 1.0f / (float) juce::jmax (1.0, sampleRate * releaseParam->load() * 0.001);
    const int   holdSamples = (int) std::lround (sampleRate * holdParam->load() * 0.001);

    const float peakDecay = (float) std::exp (-1.0 / (sampleRate * peakDecaySeconds));

    const int rmsLength = juce::jmin ((int) rmsBuffer.size() - 1,
                                      (int) std::lround (sampleRate * rmsParam->load() * 0.001));

    const int lockoutSamples = (int) std::lround (sampleRate * lockoutParam->load() * 0.001);
    const int fixedVelocity  = (int) std::lround (velocityParam->load());
    const bool sendMidi      = sendMidiParam->load() >= 0.5f;
    const int windowSamples  = (fixedVelocity > 0 || ! sendMidi)
                                 ? 0
                                 : (int) std::lround (sampleRate * velWindowParam->load() * 0.001);

    const int note     = juce::jlimit (0, 127, (int) std::lround (noteParam->load()));
    const int channel  = juce::jlimit (1, 16,  (int) std::lround (channelParam->load()));
    const bool usePort = virtualPort != nullptr && portParam->load() >= 0.5f;

    const int delaySize    = (int) delayBuffer[0].size();
    const int delaySamples = juce::jlimit (0, juce::jmax (0, delaySize - 1),
                                           (int) std::lround (sampleRate * preOpenParam->load() * 0.001));
    const bool preview     = previewParam->load() >= 0.5f;

    wetGain.setTargetValue (juce::Decibels::decibelsToGain (wetParam->load(), mixMinusInfDb));
    dryGain.setTargetValue (juce::Decibels::decibelsToGain (dryParam->load(), mixMinusInfDb));
    noiseGain.setTargetValue (juce::Decibels::decibelsToGain (noiseParam->load(), mixMinusInfDb));

    // Filtros do detector: só recalcula os coeficientes quando o knob muda.
    const float lowpassHz  = lowpassParam->load();
    const float highpassHz = highpassParam->load();
    const bool  useLowpass  = lowpassHz < lowpassOffHz;
    const bool  useHighpass = highpassHz >= highpassOffHz;

    if (! juce::exactlyEqual (lowpassHz, lastLowpassHz))
    {
        lowpassFilter.setLowpass (sampleRate, lowpassHz);
        lastLowpassHz = lowpassHz;
    }

    if (! juce::exactlyEqual (highpassHz, lastHighpassHz))
    {
        highpassFilter.setHighpass (sampleRate, highpassHz);
        lastHighpassHz = highpassHz;
    }

    if (rmsLength != lastRmsLength) // trocou o tamanho da janela RMS: recomeça
    {
        std::fill (rmsBuffer.begin(), rmsBuffer.end(), 0.0f);
        rmsSum = 0.0;
        rmsPos = 0;
        lastRmsLength = rmsLength;
    }

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
    // Funções auxiliares: mandam o evento MIDI para os DOIS caminhos de saída.
    auto sendNoteOn = [&] (int samplePos, int velocity)
    {
        midiMessages.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) velocity), samplePos);

        if (usePort)
            virtualPort->push ((juce::uint8) (0x90 | (channel - 1)), (juce::uint8) note, (juce::uint8) velocity);

        heldNote    = note;
        heldChannel = channel;
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

    //--------------------------------------------------------------------------
    // 2) Se houve reset (transporte parou, etc.), fecha a nota presa e zera tudo.
    if (pendingReset.exchange (false))
    {
        if (heldNote >= 0)
            sendNoteOff (0);

        state       = State::closed;
        level       = 0.0f;
        gateGain    = 0.0f;
        lockoutLeft = 0;
        windowLeft  = 0;
        holdLeft    = 0;
        lowpassFilter.clear();
        highpassFilter.clear();
        std::fill (rmsBuffer.begin(), rmsBuffer.end(), 0.0f);
        rmsSum = 0.0;
        rmsPos = 0;
        for (auto& d : delayBuffer)
            std::fill (d.begin(), d.end(), 0.0f);
        delayPos = 0;
    }

    //--------------------------------------------------------------------------
    // 3) Processa amostra por amostra: a precisão do disparo é de 1 amostra.
    for (int i = 0; i < numSamples; ++i)
    {
        // ---- Detector: soma L+R, filtros e medição do nível ----
        float mono = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            mono += data[ch][i];
        mono *= 1.0f / (float) juce::jmax (1, numChannels);

        float detector = mono;
        if (useHighpass) detector = highpassFilter.process (detector);
        if (useLowpass)  detector = lowpassFilter.process (detector);

        if (rmsLength > 0)
        {
            // RMS: soma móvel dos quadrados das últimas "rmsLength" amostras.
            const float square = detector * detector;
            rmsSum += (double) square - (double) rmsBuffer[(size_t) rmsPos];
            rmsBuffer[(size_t) rmsPos] = square;

            if (++rmsPos >= rmsLength)
                rmsPos = 0;

            level = std::sqrt ((float) juce::jmax (0.0, rmsSum) / (float) rmsLength);
        }
        else
        {
            // Pico: sobe na hora, desce com uma curva exponencial.
            const float magnitude = std::abs (detector);
            level = magnitude > level ? magnitude : level * peakDecay;
        }

        if (lockoutLeft > 0)
            --lockoutLeft;

        // ---- Máquina de estados do gate ----
        switch (state)
        {
            case State::closed:
                if (level >= openLevel && lockoutLeft == 0)
                {
                    if (! sendMidi)
                    {
                        state    = State::open;
                        holdLeft = holdSamples;
                    }
                    else if (fixedVelocity > 0)
                    {
                        sendNoteOn (i, fixedVelocity);   // dispara imediatamente
                        state    = State::open;
                        holdLeft = holdSamples;
                    }
                    else if (windowSamples <= 0)
                    {
                        sendNoteOn (i, velocityFromPeak (level));
                        state    = State::open;
                        holdLeft = holdSamples;
                    }
                    else
                    {
                        // Espera alguns ms medindo o pico, para a velocity
                        // refletir a força real do golpe. O áudio já abre.
                        state      = State::measuring;
                        windowPeak = level;
                        windowLeft = windowSamples;
                    }
                }
                break;

            case State::measuring:
                windowPeak = juce::jmax (windowPeak, level);

                if (--windowLeft <= 0)
                {
                    sendNoteOn (i, velocityFromPeak (windowPeak));
                    state    = State::open;
                    holdLeft = holdSamples;
                }
                break;

            case State::open:
                if (level >= closeLevel)
                {
                    holdLeft = holdSamples;       // ainda alto: renova o Hold
                }
                else if (holdLeft-- <= 0)         // abaixo do limiar e o Hold acabou
                {
                    if (heldNote >= 0)
                        sendNoteOff (i);

                    state       = State::closed;
                    lockoutLeft = lockoutSamples;
                }
                break;
        }

        // ---- Ganho do áudio: sobe no Attack, desce no Release ----
        if (state != State::closed)
            gateGain = juce::jmin (1.0f, gateGain + attackStep);
        else
            gateGain = juce::jmax (0.0f, gateGain - releaseStep);

        // ---- Áudio de saída ----
        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;
        const float noiseSample = (float) (juce::int32) noiseState * 4.6566129e-10f; // -1..1

        const float wet = wetGain.getNextValue();
        const float dry = dryGain.getNextValue();
        const float nz  = noiseGain.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float in = data[ch][i];
            float delayed  = in;

            if (delaySamples > 0)
            {
                // Pre-open: grava a amostra atual e lê a de "delaySamples" atrás.
                auto& line = delayBuffer[(size_t) ch];
                line[(size_t) delayPos] = in;

                int readPos = delayPos - delaySamples;
                if (readPos < 0)
                    readPos += delaySize;

                delayed = line[(size_t) readPos];
            }

            data[ch][i] = preview ? detector
                                  : (wet * gateGain + dry) * delayed + nz * gateGain * noiseSample;
        }

        if (delaySamples > 0 && ++delayPos >= delaySize)
            delayPos = 0;
    }
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

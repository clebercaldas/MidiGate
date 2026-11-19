# MidiGate

Plugin de gate acionado por MIDI (AU e VST3), feito com JUCE.
Note On abre o gate, Note Off fecha. Attack e Release suavizam a transição
para evitar estalos, e Floor define quanto o sinal é reduzido com o gate fechado.

## Estrutura

```
MidiGate/
├── CMakeLists.txt
└── Source/
    ├── PluginProcessor.h
    └── PluginProcessor.cpp
```

A interface usa o `juce::GenericAudioProcessorEditor` (sliders automáticos),
por isso não há PluginEditor.

## Como compilar no macOS

Pré-requisitos: Xcode (ou Command Line Tools: `xcode-select --install`),
CMake 3.22+ (`brew install cmake`) e Git. O JUCE é baixado automaticamente.

```bash
# 1. Entre na pasta do projeto
cd MidiGate

# 2. Gere o projeto (primeira vez baixa o JUCE, demora um pouco)
cmake -B build -G Xcode
#   alternativa sem Xcode IDE: cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Compile em Release
cmake --build build --config Release

# 4. (Opcional) valide o Audio Unit
auval -v aumf Mgat Rgrj
```

Os plugins são copiados automaticamente para:

- AU: `~/Library/Audio/Plug-Ins/Components/MidiGate.component`
- VST3: `~/Library/Audio/Plug-Ins/VST3/MidiGate.vst3`

Também ficam em `build/MidiGate_artefacts/Release/`.

Se a DAW não enxergar o AU novo, rode `killall -9 AudioComponentRegistrar`
e reabra a DAW.

## Windows (VST3)

O GitHub Actions também compila o VST3 para Windows. Baixe `MidiGate-Windows`
na aba Actions, extraia o zip e copie a pasta `MidiGate.vst3` para
`C:\Program Files\Common Files\VST3`. Depois peça à DAW para
reescanear os plugins.

Para compilar localmente no Windows (Visual Studio 2022 e CMake instalados):

```
cmake -B build
cmake --build build --config Release
```

## Uso

- **Logic Pro:** o AU é do tipo *MIDI-controlled effect* (`aumf`). Insira-o
  na trilha de áudio e escolha a fonte MIDI no menu "Side Chain" no topo da
  janela do plugin.
- **Reaper / Ableton / outras:** envie MIDI de uma trilha para a trilha que
  contém o plugin (send MIDI ou roteamento de entrada MIDI).

## Parâmetros

| Parâmetro | Faixa | Padrão | Função |
|-----------|-------|--------|--------|
| Attack | 0.1 a 500 ms | 2 ms | Tempo de abertura do gate |
| Release | 1 a 5000 ms | 50 ms | Tempo de fechamento do gate |
| Floor | -inf a 0 dB | -inf | Nível com o gate fechado |

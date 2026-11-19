# MidiGate e AudioTrigger

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

## AudioTrigger (áudio gera MIDI, em tempo real)

O inverso do MidiGate. Escuta o áudio ao vivo e, quando o sinal passa do
threshold, envia um Note On. Quando o sinal cai, envia o Note Off. O áudio passa
sem alteração. Serve para tocar um instrumento (por exemplo, uma bateria
sampleada) a partir de um microfone ou pad ao vivo.

A detecção é feita amostra por amostra. Com velocity dinâmica o Note On sai
cerca de 2 ms depois do golpe (a "Velocity Window", usada para medir a força).
Com velocity fixa maior que 0, sai na própria amostra em que o sinal passa do
threshold.

| Parâmetro | Função |
|-----------|--------|
| Threshold | Nível que abre o gate e dispara o Note On |
| Hysteresis | Quantos dB abaixo do threshold o gate fecha (Note Off) |
| Release | Velocidade com que o detector cai depois de um golpe |
| Retrigger Lockout | Tempo mínimo entre dois disparos |
| Velocity Window | Tempo de medição do pico para calcular a velocity |
| Note / MIDI Channel | Nota e canal enviados |
| Fixed Velocity | 0 = acompanha a força do golpe, 1 a 127 = fixa |
| Send to Virtual MIDI Port | Liga a porta virtual descrita abaixo |

### Como o MIDI sai do plugin
1. **Saída MIDI do plugin:** funciona em DAWs que encaminham a saída MIDI de
   plugins para outras trilhas (por exemplo, o Reaper).
2. **Porta MIDI virtual (macOS):** o plugin cria a porta **AudioTrigger Out**.
   No Logic Pro, uma trilha de instrumento recebe MIDI dessa porta (entrada
   MIDI da trilha em "All" ou na porta). O Logic não encaminha a saída MIDI de
   plugins de áudio para outras trilhas, e por isso existe esse caminho.
   No Windows a porta virtual não é criada.

### Uso no Logic Pro
1. Insira o **AudioTrigger** (Audio FX) na trilha de áudio com o sinal ao vivo.
2. Crie uma trilha de instrumento com o som que deve ser tocado.
3. Confirme que a trilha de instrumento recebe MIDI da porta **AudioTrigger Out**.

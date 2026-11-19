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

## AudioTrigger (gate em tempo real que envia MIDI, no estilo do ReaGate)

Um gate completo, com o modelo do **ReaGate** do Reaper (que não existe no Logic
Pro). Quando o sinal passa do threshold o gate abre: o áudio sobe e o plugin
envia um **Note On** configurável. Quando o sinal cai, o gate fecha e o plugin
envia o **Note Off**. A detecção é feita amostra por amostra.

| Grupo | Parâmetro | Função |
|-------|-----------|--------|
| Envelope | Threshold | Nível que abre o gate |
| | Hysteresis | Quantos dB abaixo do threshold o gate fecha |
| | Pre-open | Atrasa o áudio para o gate abrir antes do golpe (o plugin informa a latência à DAW) |
| | Attack / Hold / Release | Tempos do envelope do áudio. O Hold mantém o gate aberto depois que o nível cai |
| Detector | Lowpass / Highpass | Filtros só do sinal do detector. Útil para ignorar vazamento de outras peças |
| | RMS size | 0 = detecção por pico. Acima de 0, usa o nível RMS nessa janela |
| | Preview filter output | Você ouve o sinal filtrado do detector |
| Saída | Wet / Dry / Noise | Mix do áudio gateado, do áudio direto e de ruído com o gate aberto |
| MIDI | Send MIDI on open/close | Liga e desliga o envio de MIDI |
| | Note / MIDI Channel | Nota e canal enviados |
| | Fixed Velocity | 0 = a velocity vem da amplitude do golpe, 1 a 127 = fixa |
| | Velocity Low / High / Curve | Pico em dB que vira velocity 1 e 127, e o formato da curva |
| | Velocity Window | Tempo de medição do pico para calcular a velocity |
| | Retrigger Lockout | Tempo mínimo entre dois disparos |
| | Send to Virtual MIDI Port | Liga a porta virtual descrita abaixo |

Com velocity pela amplitude, o Note On sai cerca de 2 ms depois do golpe (a
Velocity Window). Com Fixed Velocity maior que 0, sai na própria amostra em que
o sinal passa do threshold.

Ainda não tem: a entrada de sidechain do detector (Detector input).

### Como o MIDI sai do plugin
1. **Saída MIDI do plugin:** funciona em DAWs que encaminham a saída MIDI de
   plugins para outras trilhas (por exemplo, o Reaper).
2. **Porta MIDI virtual (macOS):** o plugin cria a porta **AudioTrigger Out**
   (e **AudioTrigger Out 2**, e assim por diante, uma por instância). No Logic
   Pro, uma trilha de instrumento recebe MIDI dessa porta. O Logic não
   encaminha a saída MIDI de plugins de áudio para outras trilhas, e por isso
   existe esse caminho. No Windows a porta virtual não é criada.

### Uso no Logic Pro
1. Insira o **AudioTrigger** (Audio FX) na trilha de áudio com o sinal ao vivo.
2. Crie uma trilha de instrumento com o som que deve ser tocado.
3. Confirme que a trilha de instrumento recebe MIDI da porta **AudioTrigger Out**.
4. Se quiser só o MIDI e não o áudio do gate, ponha **Wet** em -inf.

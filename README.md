# QF-ML

Reusable ML integration library for C++/Qt6 apps. One façade
(`InferenceService`) over speech-to-text, wake word, embeddings, LLM
inference, and text-to-speech.

**Status: pre-alpha, not suitable for production.** APIs change. Expect breakage.

## Who it's for

C++/Qt6 developers who want ML capabilities in their app without
integrating NeMo, llama.cpp, ONNX Runtime, and a TTS engine themselves.

## Capabilities

| | Local runtime | Remote |
|---|---|---|
| Speech-to-text | NeMo-Speech.cpp (in-process) | planned |
| Wake word | ONNX Runtime, openWakeWord | n/a |
| Embeddings | ONNX Runtime, MiniLM INT8 | n/a |
| LLM | llama.cpp in Docker | OpenAI-compatible HTTP |
| Text-to-speech | HeadTTS (Node) | planned |

"Remote" means an HTTP endpoint. "Local" means an in-process runtime or
a Docker-managed server. Both are driven through the same signals.

## Contents

- [Install](#install)
- [Use as a submodule](#use-as-a-submodule)
- [Configure your CMake](#configure-your-cmake)
- [Initialize the service](#initialize-the-service)
- [Build](#build)
- [Supported systems](#supported-systems)
- [Environment variables](#environment-variables)
- [Signals](#signals)

---

## Install

Debian 12 / Ubuntu 24.04:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git curl \
  qt6-base-dev qt6-multimedia-dev qt6-websockets-dev \
  qt6-webchannel-dev qt6-speech-dev \
  libavfilter-dev libavformat-dev libavutil-dev \
  libvulkan-dev glslang-tools docker.io
```

Node.js 20+ (for TTS):

```bash
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs
```

Hugging Face CLI (optional, faster model downloads):

```bash
curl -LsSf https://hf.co/cli/install.sh | bash
```

---

## Use as a submodule

From your project root:

```bash
git submodule add https://github.com/Sujan-Naik/QF-ML external/QF-ML
git submodule update --init --recursive
```

Mark QF-ML and its own submodules as dirty so your repo doesn't track
their internal state:

```bash
git config -f .gitmodules submodule.external/QF-ML.ignore dirty
git config -f .gitmodules submodule.external/QF-ML/NeMo-Speech.cpp.ignore dirty
git config -f .gitmodules submodule.external/QF-ML/external/NeMo-Speech.cpp.ignore dirty
git add .gitmodules
git commit -m "Add QF-ML as submodule"
```

If you already vendored QF-ML, skip `submodule add` and just set the
ignore flags:

```bash
git config -f .gitmodules submodule.external/QF-ML.ignore dirty
git config -f .gitmodules submodule.external/QF-ML/external/NeMo-Speech.cpp.ignore dirty
```

---

## Configure your CMake

Add to your top-level `CMakeLists.txt`:

```cmake
add_subdirectory(external/QF-ML)

target_link_libraries(your-app PRIVATE qf-inference)
```

`qf-inference` exports Qt6 Core, Gui, Widgets, Network, Multimedia,
TextToSpeech, and WebSockets publicly. Your app does not need to
`find_package` them separately.

If your app already finds Qt6, QF-ML reuses it. If not, QF-ML's own
`find_package(Qt6 ...)` runs first.

---

## Initialize the service

Copy this into your `main.cpp`. It is the same shape as QF-ML's own
`src/main.cpp`.

```cpp
#include <QApplication>
#include <QDebug>

#include "inference/InferenceService.h"
#include "inference/LlamaManager.h"

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  InferenceService service;

  // Local LLM in Docker, local ASR, local TTS.
  const bool ok = service.initialize(
      LlamaManager::Backend::Vulkan,          // llama.cpp backend
      QString(),                              // STT path; empty = auto
      InferenceService::SttModel::Nemotron35, // ASR model
      InferenceService::LlmConfig{});         // default = Local

  if (!ok) {
    qWarning() << "QF-ML failed to initialize.";
    return 1;
  }

  QObject::connect(&service, &InferenceService::serviceError,
                   [](const QString &e) { qWarning() << "QF-ML:" << e; });

  // Chat request. Returns a token; deltas arrive on llmDelta.
  QJsonArray messages;
  messages.append(QJsonObject{
      {"role", "user"},
      {"content", "Hello."},
  });

  const auto token = service.sendChatRequest(messages);

  QObject::connect(&service, &InferenceService::llmDelta,
                   [](const InferenceService::RequestToken &t, const QString &d) {
                     Q_UNUSED(t);
                     qDebug().noquote() << d;
                   });

  return app.exec();
}
```

### Remote LLM instead of Docker

```cpp
InferenceService::LlmConfig cfg;
cfg.mode     = InferenceService::LlmMode::Remote;
cfg.endpoint = "https://openrouter.ai/api/v1/chat/completions";
cfg.model    = "anthropic/claude-3.5-sonnet";
cfg.authType = InferenceService::LlmAuthType::Bearer;
cfg.apiKey   = qEnvironmentVariable("OPENROUTER_API_KEY");

service.initialize(LlamaManager::Backend::Vulkan,
                   QString(),
                   InferenceService::SttModel::Nemotron35,
                   cfg);
```

### Speech-to-text

```cpp
QObject::connect(&service, &InferenceService::liveSegment,
                 [](const QString &text, bool isFinal) {
                   qDebug() << (isFinal ? "[final]" : "[interim]") << text;
                 });

service.startSttStreaming();
// feed PCM with service.feedSttAudio(pcm);
service.stopSttStreaming();
```

### Text-to-speech

```cpp
service.setTtsEnabled(true);
service.speak("Hello, world.");
```

### Embeddings

```cpp
const std::vector<float> v = service.embed("some text");
```

---

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/qf-ml-demo
```

To build the QF-ML demo standalone (without your app):

```bash
git clone --recursive https://github.com/Sujan-Naik/QF-ML
cd QF-ML
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/qf-ml-demo
```

---

## Supported systems

| Platform | Status |
|---|---|
| Linux x86_64 (Debian 12, Ubuntu 24.04) | Primary |
| Linux aarch64 | Untested |
| macOS 13+ arm64 / x86_64 | Expected; CPU-only ASR unless `GGML_METAL=ON` |
| Windows 10/11 x86_64 | Expected; use Qt's MSVC 2019 kit |

Baseline: **Qt 6.4.2**, CMake ≥ 3.26, C++17.

Requires Docker for local LLM. Requires Node 20+ for local TTS.

---

## Environment variables

| Var | Purpose |
|---|---|
| `TALOS_LLM_MODE` | `local` (default) or `remote` |
| `TALOS_LLM_URL` | Remote endpoint |
| `TALOS_LLM_MODEL` | Remote model ID |
| `TALOS_LLM_API_KEY` | Bearer token |
| `TALOS_LLM_AUTH` | `none` or `bearer` |
| `TALOS_LLM_BACKEND` | `rocm`, `cuda`, `vulkan` (default), `intel`, `cpu` |
| `QF_STT_MODEL` | `nemotron-3.5`, `nemotron-en`, `parakeet-tdt`, `parakeet-ctc` |
| `QF_STT_MODEL_PATH` | Explicit ASR gguf path |
| `QF_STT_GPU` | GPU index (default `0`) |
| `QF_NODE_CLI` | Node binary override for HeadTTS |
| `QF_HEADTTS_DIR` | HeadTTS directory override |

---

## Signals

```cpp
// LLM
llmDelta(RequestToken, QString)
llmFinished(RequestToken)
llmToolCalls(RequestToken, QJsonArray)
llmError(RequestToken, QString)
llmReady()
llmConfigurationChanged()

// STT
transcriptionFinished(QString)
liveSegment(QString, bool isFinal)
sttStreamOpened()
sttStreamClosed()
sttModelChanged(QString)

// TTS
ttsReady()
ttsSentenceFinished()
ttsVoiceChanged(QString)
ttsVoicesChanged(QStringList)
ttsError(QString)

// Embeddings
embedderReady()
embedderError(QString)

// Models
remoteLlmModelsChanged()
remoteLlmVariantsChanged(QString)
selectedLlmModelChanged(QString)
modelDownloadStarted(QString)
modelDownloadProgress(QString, qint64, qint64)
modelDownloadFinished(QString)
modelDownloadError(QString, QString)

// Misc
serviceError(QString)
```

---

## Model storage

All models live under a per-user data root:

| Platform | Path |
|---|---|
| Linux | `$XDG_DATA_HOME/Questfarer/qf-ml` or `~/.local/share/Questfarer/qf-ml` |
| macOS | `~/Library/Application Support/Questfarer/qf-ml` |
| Windows | `%LOCALAPPDATA%/Questfarer/qf-ml` |

Subdirectories: `models/llm`, `models/stt`, `models/wakeword`,
`models/embedding`. QF-ML downloads into these paths on first use.

---

## License

See `LICENSE`. Third-party components keep their own licenses.
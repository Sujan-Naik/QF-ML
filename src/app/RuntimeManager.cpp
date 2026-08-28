#include "../../include/app/RuntimeManager.h"

#include "inference/LlamaManager.h"
#include "voice/TtsManager.h"


RuntimeManager::RuntimeManager(QObject *parent)
    : QObject(parent)
      , m_llamaManager(
          std::make_unique<LlamaManager>(this)
      )
      , m_ttsManager(
          std::make_unique<TtsManager>(this)
      ) {
    connect(
        m_llamaManager.get(),
        &LlamaManager::errorOccurred,
        this,
        &RuntimeManager::runtimeError
    );

    connect(
        m_ttsManager.get(),
        &TtsManager::errorOccurred,
        this,
        &RuntimeManager::runtimeError
    );
}


RuntimeManager::~RuntimeManager() = default;


LlamaManager *RuntimeManager::llama() const {
    return m_llamaManager.get();
}


TtsManager *RuntimeManager::tts() const {
    return m_ttsManager.get();
}

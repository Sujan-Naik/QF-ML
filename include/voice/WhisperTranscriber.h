#pragma once

#include <QObject>
#include <QString>
#include <thread>
#include <vector>

#include "ITranscriber.h"
#include "whisper.h"

class WhisperTranscriber : public QObject, public ITranscriber {
  Q_OBJECT

public:
  explicit WhisperTranscriber(const QString &modelPath,
                              QObject *parent = nullptr)
      : QObject(parent) {
    whisper_context_params cparams = whisper_context_default_params();

    m_ctx = whisper_init_from_file_with_params(modelPath.toUtf8().constData(),
                                               cparams);
  }

  ~WhisperTranscriber() override {
    if (m_ctx) {
      whisper_free(m_ctx);
    }
  }

  bool isLoaded() const override { return m_ctx != nullptr; }

  QString transcribe(const std::vector<float> &pcm32f) override {
    if (!m_ctx || pcm32f.empty()) {
      return {};
    }

    whisper_full_params params =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    params.print_progress = false;
    params.print_special = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.translate = false;
    params.language = "en";
    params.n_threads = 4;

    if (whisper_full(m_ctx, params, pcm32f.data(), pcm32f.size()) != 0) {
      return {};
    }

    QString resultText;

    const int n_segments = whisper_full_n_segments(m_ctx);

    for (int i = 0; i < n_segments; ++i) {
      const char *text = whisper_full_get_segment_text(m_ctx, i);

      if (text) {
        resultText += QString::fromUtf8(text);
      }
    }

    return resultText.trimmed();
  }

  void transcribeAsync(const std::vector<float> &pcm32f) {
    // See threading note below.
    std::thread([this, pcm32f]() {
      const QString resultText = transcribe(pcm32f);

      emit transcriptionFinished(resultText);
    }).detach();
  }

signals:
  void transcriptionFinished(const QString &text);

private:
  whisper_context *m_ctx = nullptr;
};

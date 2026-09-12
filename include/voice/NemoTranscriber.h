#pragma once

#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

#include <vector>

#include "ITranscriber.h"
#include "nemo_speech/asr.h"

class NemoTranscriber : public QObject, public ITranscriber {
  Q_OBJECT

public:
  explicit NemoTranscriber(const QString &modelPath, int gpu = 0,
                           const QString &language = QString(),
                           QObject *parent = nullptr);

  ~NemoTranscriber() override;

  bool isLoaded() const override;

  QString transcribe(const std::vector<float> &pcm32f) override;

  QString modelPath() const;

  QString language() const;

  void setLanguage(const QString &language);

signals:
  void transcriptionFinished(const QString &text);

  void transcriptionError(const QString &error);

private:
  nemo_speech_asr_recognizer *m_recognizer = nullptr;

  QString m_modelPath;
  QString m_language;

  // Temporary local HTTP endpoint for testing.
  // POST raw MP4 bytes to:
  // http://127.0.0.1:8080/transcribe
  QTcpServer m_httpServer;
};
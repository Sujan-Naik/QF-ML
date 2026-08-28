#pragma once

#include <QObject>
#include <QString>
#include <vector>

#include "ITranscriber.h"
#include "parakeet_capi.h"

class ParakeetTranscriber
        : public QObject
          , public ITranscriber {
    Q_OBJECT

public:
    explicit ParakeetTranscriber(
        const QString &modelPath,
        QObject *parent = nullptr
    );

    ~ParakeetTranscriber() override;

    bool isLoaded() const override;

    QString transcribe(
        const std::vector<float> &pcm32f
    ) override;

signals:
    void transcriptionFinished(
        const QString &text
    );

    void transcriptionError(
        const QString &error
    );

private:
    parakeet_ctx *m_ctx = nullptr;
};

#pragma once

#include <QStandardPaths>
#include <QDir>

namespace QFPaths {

inline QString dataRoot() {
    const QString base =
        QStandardPaths::writableLocation(
            QStandardPaths::GenericDataLocation
        );

    return QDir(base).filePath(
        QStringLiteral("QuestFarer")
    );
}

inline QString qfmlRoot() {
    return QDir(dataRoot()).filePath(
        QStringLiteral("qf-ml")
    );
}

inline QString modelsRoot() {
    return QDir(qfmlRoot()).filePath(
        QStringLiteral("models")
    );
}

inline QString llmModelsDir() {
    return QDir(modelsRoot()).filePath(
        QStringLiteral("llm")
    );
}

inline QString sttModelsDir() {
    return QDir(modelsRoot()).filePath(
        QStringLiteral("stt")
    );
}

inline QString whisperModelsDir() {
    return QDir(modelsRoot()).filePath(
        QStringLiteral("whisper")
    );
}

inline QString wakewordModelsDir() {
    return QDir(modelsRoot()).filePath(
        QStringLiteral("wakeword")
    );
}

}
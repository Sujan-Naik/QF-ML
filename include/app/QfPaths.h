// QFPaths.h
#pragma once
#include <QStandardPaths>
#include <QDir>

namespace QFPaths {

    inline QString dataRoot() {
        const QString base = QStandardPaths::writableLocation(
            QStandardPaths::GenericDataLocation
        );
        return QDir(base).filePath(QStringLiteral("qf-inference"));
    }

    inline QString modelsRoot() {
        return QDir(dataRoot()).filePath(QStringLiteral("models"));
    }

    inline QString llmModelsDir()      { return QDir(modelsRoot()).filePath("llm"); }
    inline QString sttModelsDir()      { return QDir(modelsRoot()).filePath("stt"); }
    inline QString whisperModelsDir()  { return QDir(modelsRoot()).filePath("whisper"); }
    inline QString wakewordModelsDir() { return QDir(modelsRoot()).filePath("wakeword"); }

}
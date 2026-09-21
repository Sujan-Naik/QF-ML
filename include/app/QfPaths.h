#pragma once

#include <QDir>
#include <QStandardPaths>

namespace QFPaths {

// The Lore application data root.
//
//   Linux:   $XDG_DATA_HOME/Questfarer, or $HOME/.local/share/Questfarer
//   macOS:   $HOME/Library/Application Support/Questfarer
//   Windows: %LOCALAPPDATA%/Questfarer
//
// Qt's GenericDataLocation maps to:
//
//   Linux:   $XDG_DATA_HOME, or $HOME/.local/share
//   macOS:   $HOME/Library/Application Support
//   Windows: %LOCALAPPDATA%
//
// so appending "Questfarer" reproduces the root on every platform.
inline QString dataRoot() {
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);

  return QDir(base).filePath(QStringLiteral("Questfarer"));
}

// Everything QF-ML produces lives under <dataRoot>/qf-ml.
inline QString qfMlRoot() {
  return QDir(dataRoot()).filePath(QStringLiteral("qf-ml"));
}

// Models live under <qfMlRoot>/models. QF-ML downloads into this
// directory, and every accessor below is a subdirectory of it.
inline QString modelsRoot() {
  return QDir(qfMlRoot()).filePath(QStringLiteral("models"));
}

inline QString llmModelsDir() {
  return QDir(modelsRoot()).filePath(QStringLiteral("llm"));
}

inline QString sttModelsDir() {
  return QDir(modelsRoot()).filePath(QStringLiteral("stt"));
}

inline QString whisperModelsDir() {
  return QDir(modelsRoot()).filePath(QStringLiteral("whisper"));
}

inline QString wakewordModelsDir() {
  return QDir(modelsRoot()).filePath(QStringLiteral("wakeword"));
}

} // namespace QFPaths
#pragma once

#include <QDir>
#include <QStandardPaths>

namespace QFPaths {

// The shared QF data root. Must match the QF-ML CMake variable
// QF_DATA_ROOT, which QF-ML computes as:
//
//   Linux:   $XDG_DATA_HOME/qf-inference, or $HOME/.local/share/qf-inference
//   macOS:   $HOME/Library/Application Support/qf-inference
//   Windows: %LOCALAPPDATA%/qf-inference
//
// Qt's GenericDataLocation maps to:
//
//   Linux:   $XDG_DATA_HOME, or $HOME/.local/share
//   macOS:   $HOME/Library/Application Support
//   Windows: %LOCALAPPDATA%
//
// so appending "qf-inference" reproduces QF-ML's QF_DATA_ROOT on every
// platform. If QF-ML's CMake changes that name, this must change with
// it. The two are one contract.
inline QString dataRoot() {
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);

  return QDir(base).filePath(QStringLiteral("qf-inference"));
}

// Models live under the shared data root. QF-ML downloads into
// <dataRoot>/models, and every accessor below is a subdirectory of it.
inline QString modelsRoot() {
  return QDir(dataRoot()).filePath(QStringLiteral("models"));
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
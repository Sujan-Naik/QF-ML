#ifndef RUNTIMEMANAGER_H
#define RUNTIMEMANAGER_H

#include <QObject>

#include <memory>
#include <qt6/QtCore/qtmetamacros.h>

class LlamaManager;
class TtsManager;

class RuntimeManager : public QObject {
    Q_OBJECT

public:
    explicit RuntimeManager(QObject *parent = nullptr);

    ~RuntimeManager() override;

    LlamaManager *llama() const;

    TtsManager *tts() const;

signals:
    void runtimeError(
        const QString &error
    );

private:
    std::unique_ptr<LlamaManager> m_llamaManager;
    std::unique_ptr<TtsManager> m_ttsManager;
};

#endif // RUNTIMEMANAGER_H

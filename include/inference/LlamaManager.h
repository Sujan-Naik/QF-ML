#pragma once

#include <QNetworkAccessManager>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QByteArray>
#include <QString>

class LlamaManager final : public QObject {
    Q_OBJECT

public:
    enum class Backend {
        Rocm,
        Cuda,
        Vulkan,
        Intel,
        Cpu
    };

    Q_ENUM(Backend)

    explicit LlamaManager(QObject *parent = nullptr);

    ~LlamaManager() override;

    bool initialize(
        const QString &modelPath,
        Backend backend,
        bool autoStart = true
    );

    bool configure(
        const QString &modelPath,
        Backend backend
    );

    void setEnabled(bool enabled);

    bool isEnabled() const;

    bool isRunning() const;

    bool isReady() const;

    QUrl serverUrl() const;

    QString modelPath() const;

    Backend backend() const;

    void start();

    void stop();

signals:
    void serverStarting();

    void serverReady();

    void serverStopped();

    void statusChanged();

    void errorOccurred(
        const QString &error
    );

private slots:
    void checkServerHealth();

    void onDockerPullFinished(
        int exitCode,
        QProcess::ExitStatus status
    );

    void onDockerRunFinished(
        int exitCode,
        QProcess::ExitStatus status
    );

    void onDockerOutputReady();

    void onDockerErrorReady();

private:
    bool checkDockerAvailable();

    QString dockerImage() const;

    QStringList dockerRunArguments() const;

    void startDockerContainer();

    void pullDockerImage();

    void runDockerContainer();

    void stopDockerContainer();

private:
    static constexpr int HOST_PORT = 8081;
    static constexpr int CONTAINER_PORT = 8081;
    static constexpr int MAX_HEALTH_ATTEMPTS = 60;

    static constexpr const char *CONTAINER_NAME =
            "talos-llama";

    /*
     * These should match the images you already use.
     */
    static constexpr const char *ROCM_IMAGE =
            "ghcr.io/ggml-org/llama.cpp:server-rocm";

    static constexpr const char *CUDA_IMAGE =
            "ghcr.io/ggml-org/llama.cpp:server-cuda";

    static constexpr const char *VULKAN_IMAGE =
            "ghcr.io/ggml-org/llama.cpp:server-vulkan";

    static constexpr const char *INTEL_IMAGE =
            "ghcr.io/ggml-org/llama.cpp:server";

    static constexpr const char *CPU_IMAGE =
            "ghcr.io/ggml-org/llama.cpp:server";

    QNetworkAccessManager *m_networkManager = nullptr;
    QTimer *m_healthCheckTimer = nullptr;
    QProcess *m_dockerProcess = nullptr;

    /*
     * Docker output must be buffered because
     * readyReadStandardOutput() consumes the bytes.
     */
    QByteArray m_dockerStdout;
    QByteArray m_dockerStderr;

    QString m_modelPath;
    QString m_containerId;

    QUrl m_serverUrl;

    Backend m_backend =
            Backend::Cpu;

    bool m_initialized = false;
    bool m_enabled = true;
    bool m_containerStarted = false;
    bool m_ready = false;

    int m_healthAttempts = 0;
};

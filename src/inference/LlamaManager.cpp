#include "../../include/inference/LlamaManager.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>


LlamaManager::LlamaManager(QObject *parent)
    : QObject(parent)
      , m_networkManager(
          new QNetworkAccessManager(
              this
          )
      )
      , m_healthCheckTimer(
          new QTimer(
              this
          )
      ) {
    m_networkManager->setProxy(
        QNetworkProxy::NoProxy
    );

    m_healthCheckTimer->setInterval(
        2000
    );

    connect(
        m_healthCheckTimer,
        &QTimer::timeout,
        this,
        &LlamaManager::checkServerHealth
    );
}


LlamaManager::~LlamaManager() {
    stopDockerContainer();
}


bool LlamaManager::initialize(
    const QString &modelPath,
    Backend backend,
    bool autoStart
) {
    if (
        !configure(
            modelPath,
            backend
        )
    ) {
        return false;
    }

    if (autoStart)
        start();

    return true;
}


bool LlamaManager::configure(
    const QString &modelPath,
    Backend backend
) {
    const QString cleanedPath =
            modelPath.trimmed();

    if (cleanedPath.isEmpty()) {
        emit errorOccurred(
            QStringLiteral(
                "No llama.cpp model path was provided."
            )
        );

        return false;
    }

    const QFileInfo modelInfo(
        cleanedPath
    );

    if (
        !modelInfo.exists() ||
        !modelInfo.isFile() ||
        !modelInfo.isReadable()
    ) {
        emit errorOccurred(
            QStringLiteral(
                "Llama model is not available: %1"
            ).arg(
                cleanedPath
            )
        );

        return false;
    }

    /*
     * Stop any currently running server before
     * changing configuration.
     */
    if (
        m_containerStarted ||
        !m_containerId.isEmpty()
    ) {
        stopDockerContainer();
    }

    m_healthCheckTimer->stop();

    m_ready =
            false;

    m_healthAttempts =
            0;

    m_modelPath =
            modelInfo.absoluteFilePath();

    m_backend =
            backend;

    m_serverUrl =
            QUrl(
                QStringLiteral(
                    "http://127.0.0.1:%1"
                ).arg(
                    HOST_PORT
                )
            );

    m_initialized =
            true;

    qDebug()
            << "[LLAMA] Configured"
            << "model=" << m_modelPath
            << "backend=" << static_cast<int>(
                m_backend
            )
            << "endpoint=" << m_serverUrl;

    emit statusChanged();

    return true;
}


void LlamaManager::setEnabled(
    bool enabled
) {
    if (m_enabled == enabled)
        return;

    m_enabled =
            enabled;

    qDebug()
            << "[LLAMA] setEnabled ="
            << m_enabled;

    if (m_enabled) {
        start();
    } else {
        stop();
    }

    emit statusChanged();
}


bool LlamaManager::isEnabled() const {
    return m_enabled;
}


bool LlamaManager::isRunning() const {
    return m_containerStarted;
}


bool LlamaManager::isReady() const {
    return m_ready;
}


QUrl LlamaManager::serverUrl() const {
    return m_serverUrl;
}


QString LlamaManager::modelPath() const {
    return m_modelPath;
}


LlamaManager::Backend LlamaManager::backend() const {
    return m_backend;
}


void LlamaManager::start() {
    if (!m_initialized) {
        emit errorOccurred(
            QStringLiteral(
                "LlamaManager has not been configured."
            )
        );

        return;
    }

    if (m_modelPath.isEmpty()) {
        emit errorOccurred(
            QStringLiteral(
                "No llama.cpp model is configured."
            )
        );

        return;
    }

    if (
        m_containerStarted ||
        m_ready
    ) {
        return;
    }

    if (!checkDockerAvailable()) {
        emit errorOccurred(
            QStringLiteral(
                "Docker is not available or user lacks permissions."
            )
        );

        return;
    }

    m_ready =
            false;

    m_healthAttempts =
            0;

    emit serverStarting();
    emit statusChanged();

    startDockerContainer();
}


void LlamaManager::stop() {
    m_healthCheckTimer->stop();

    const bool wasRunning =
            m_containerStarted ||
            !m_containerId.isEmpty();

    stopDockerContainer();

    m_ready =
            false;

    if (wasRunning)
        emit serverStopped();

    emit statusChanged();
}


bool LlamaManager::checkDockerAvailable() {
    QProcess check;

    check.start(
        QStringLiteral("docker"),
        QStringList()
        << QStringLiteral(
            "info"
        )
    );

    if (!check.waitForFinished(3000)) {
        qWarning()
                << "[LLAMA] Docker check timed out.";

        check.kill();
        check.waitForFinished(1000);

        return false;
    }

    if (check.exitCode() != 0) {
        qWarning()
                << "[LLAMA] Docker not available:"
                << check.readAllStandardError();

        return false;
    }

    return true;
}


QString LlamaManager::dockerImage() const {
    switch (m_backend) {
        case Backend::Rocm:
            return QStringLiteral(
                "ghcr.io/ggml-org/llama.cpp:server-rocm"
            );

        case Backend::Cuda:
            return QStringLiteral(
                "ghcr.io/ggml-org/llama.cpp:server-cuda"
            );

        case Backend::Vulkan:
            return QStringLiteral(
                "ghcr.io/ggml-org/llama.cpp:server-vulkan"
            );

        case Backend::Intel:
            return QStringLiteral(
                "ghcr.io/ggml-org/llama.cpp:server"
            );

        case Backend::Cpu:
        default:
            return QStringLiteral(
                "ghcr.io/ggml-org/llama.cpp:server"
            );
    }
}


QStringList LlamaManager::dockerRunArguments() const {
    const QFileInfo modelInfo(
        m_modelPath
    );

    const QString modelDirectory =
            modelInfo.absolutePath();

    const QString modelFileName =
            modelInfo.fileName();

    /*
     * Qwen3-30B-A3B-Instruct-2507 has a native
     * 262144-token context window.
     *
     * This deliberately requests the full context.
     */
    constexpr int CONTEXT_SIZE =
            262144;

    QStringList args;

    args
            << QStringLiteral(
                "run"
            )
            << QStringLiteral(
                "--rm"
            )
            << QStringLiteral(
                "-d"
            )
            << QStringLiteral(
                "--name"
            )
            << QString::fromUtf8(
                CONTAINER_NAME
            )
            << QStringLiteral(
                "-p"
            )
            << QStringLiteral(
                "127.0.0.1:%1:%2"
            ).arg(
                HOST_PORT
            ).arg(
                CONTAINER_PORT
            )
            << QStringLiteral(
                "-v"
            )
            << QStringLiteral(
                "%1:/models:ro"
            ).arg(
                QDir::toNativeSeparators(
                    modelDirectory
                )
            );

    switch (m_backend) {
        case Backend::Rocm:

            args
                    << QStringLiteral(
                        "--device=/dev/kfd"
                    )
                    << QStringLiteral(
                        "--device=/dev/dri"
                    )
                    << QStringLiteral(
                        "--group-add"
                    )
                    << QStringLiteral(
                        "video"
                    )
                    << QStringLiteral(
                        "--ipc=host"
                    );

            break;

        case Backend::Cuda:

            args
                    << QStringLiteral(
                        "--gpus"
                    )
                    << QStringLiteral(
                        "all"
                    );

            break;

        case Backend::Vulkan:

            args
                    << QStringLiteral(
                        "--device=/dev/dri"
                    );

            break;

        case Backend::Intel:

            args
                    << QStringLiteral(
                        "--device=/dev/dri"
                    );

            break;

        case Backend::Cpu:
        default:
            break;
    }

    args
            << dockerImage()

            // ---------------------------------------------------------------------
            // Model
            // ---------------------------------------------------------------------

            << QStringLiteral(
                "-m"
            )
            << (
                QStringLiteral(
                    "/models/"
                ) +
                modelFileName
            )

            // ---------------------------------------------------------------------
            // HTTP server
            // ---------------------------------------------------------------------

            << QStringLiteral(
                "--host"
            )
            << QStringLiteral(
                "0.0.0.0"
            )

            << QStringLiteral(
                "--port"
            )
            << QString::number(
                CONTAINER_PORT
            )

            // ---------------------------------------------------------------------
            // Context
            // ---------------------------------------------------------------------

            << QStringLiteral(
                "--ctx-size"
            )
            << QString::number(
                CONTEXT_SIZE
            )

            // ---------------------------------------------------------------------
            // Attention
            // ---------------------------------------------------------------------

            << QStringLiteral(
                "--flash-attn"
            )
            << QStringLiteral(
                "on"
            )

            // ---------------------------------------------------------------------
            // KV cache
            //
            // Q8 KV dramatically reduces the memory footprint compared with
            // the default F16 KV cache while preserving much more information
            // than a very aggressive Q4 KV configuration.
            // ---------------------------------------------------------------------

            << QStringLiteral(
                "--cache-type-k"
            )
            << QStringLiteral(
                "q8_0"
            )

            << QStringLiteral(
                "--cache-type-v"
            )
            << QStringLiteral(
                "q8_0"
            )

            << QStringLiteral(
                "--kv-offload"
            )

            // ---------------------------------------------------------------------
            // One sequence.
            //
            // Multiple parallel sequences multiply KV-cache requirements,
            // which we do not want on a 12 GB GPU / 32 GB RAM system.
            // ---------------------------------------------------------------------

            << QStringLiteral(
                "--parallel"
            )
            << QStringLiteral(
                "1"
            );

    qDebug()
            << "[LLAMA] Context:"
            << CONTEXT_SIZE;

    qDebug()
            << "[LLAMA] KV cache:"
            << "K=q8_0"
            << "V=q8_0";

    qDebug()
            << "[LLAMA] Flash attention:"
            << "on";

    return args;
}


void LlamaManager::startDockerContainer() {
    const QString image =
            dockerImage();

    QProcess check;

    check.start(
        QStringLiteral(
            "docker"
        ),
        QStringList()
        << QStringLiteral(
            "ps"
        )
        << QStringLiteral(
            "--filter"
        )
        << (
            QStringLiteral(
                "ancestor="
            ) +
            image
        )
        << QStringLiteral(
            "--format"
        )
        << QStringLiteral(
            "{{.ID}}"
        )
    );

    if (!check.waitForFinished(3000)) {
        emit errorOccurred(
            QStringLiteral(
                "Timed out while checking for an existing llama.cpp container."
            )
        );

        check.kill();
        check.waitForFinished(1000);

        return;
    }

    const QString output =
            QString::fromUtf8(
                check.readAllStandardOutput()
            ).trimmed();

    if (!output.isEmpty()) {
        const QStringList ids =
                output.split(
                    QRegularExpression(
                        QStringLiteral(
                            "\\s+"
                        )
                    ),
                    Qt::SkipEmptyParts
                );

        if (!ids.isEmpty()) {
            m_containerId =
                    ids.first();
        }

        m_containerStarted =
                !m_containerId.isEmpty();

        m_ready =
                false;

        m_healthAttempts =
                0;

        if (m_containerStarted) {
            qDebug()
                    << "[LLAMA] Found existing container:"
                    << m_containerId;

            m_healthCheckTimer->start();

            emit statusChanged();

            return;
        }
    }

    QProcess inspect;

    inspect.start(
        QStringLiteral(
            "docker"
        ),
        QStringList()
        << QStringLiteral(
            "image"
        )
        << QStringLiteral(
            "inspect"
        )
        << image
    );

    if (!inspect.waitForFinished(3000)) {
        emit errorOccurred(
            QStringLiteral(
                "Timed out while checking the llama.cpp Docker image."
            )
        );

        inspect.kill();
        inspect.waitForFinished(1000);

        return;
    }

    if (inspect.exitCode() != 0)
        pullDockerImage();
    else
        runDockerContainer();
}


void LlamaManager::pullDockerImage() {
    if (m_dockerProcess) {
        m_dockerProcess->disconnect(
            this
        );

        m_dockerProcess->deleteLater();

        m_dockerProcess =
                nullptr;
    }

    m_dockerStdout.clear();
    m_dockerStderr.clear();

    m_dockerProcess =
            new QProcess(
                this
            );

    connect(
        m_dockerProcess,
        &QProcess::finished,
        this,
        &LlamaManager::onDockerPullFinished
    );

    connect(
        m_dockerProcess,
        &QProcess::readyReadStandardOutput,
        this,
        &LlamaManager::onDockerOutputReady
    );

    connect(
        m_dockerProcess,
        &QProcess::readyReadStandardError,
        this,
        &LlamaManager::onDockerErrorReady
    );

    const QString image =
            dockerImage();

    qDebug()
            << "[LLAMA] Pulling Docker image:"
            << image;

    m_dockerProcess->start(
        QStringLiteral(
            "docker"
        ),
        QStringList()
        << QStringLiteral(
            "pull"
        )
        << image
    );
}


void LlamaManager::onDockerPullFinished(
    int exitCode,
    QProcess::ExitStatus status
) {
    if (m_dockerProcess) {
        const QByteArray remainingStdout =
                m_dockerProcess
                ->readAllStandardOutput();

        const QByteArray remainingStderr =
                m_dockerProcess
                ->readAllStandardError();

        if (!remainingStdout.isEmpty())
            m_dockerStdout +=
                    remainingStdout;

        if (!remainingStderr.isEmpty())
            m_dockerStderr +=
                    remainingStderr;
    }

    if (
        exitCode != 0 ||
        status != QProcess::NormalExit
    ) {
        QString error =
                QStringLiteral(
                    "Failed to pull llama.cpp Docker image."
                );

        const QString stderrOutput =
                QString::fromUtf8(
                    m_dockerStderr
                ).trimmed();

        if (!stderrOutput.isEmpty()) {
            error +=
                    QStringLiteral(
                        ": "
                    ) +
                    stderrOutput;
        }

        emit errorOccurred(
            error
        );

        emit statusChanged();

        return;
    }

    qDebug()
            << "[LLAMA] Docker image pulled successfully.";

    runDockerContainer();
}


void LlamaManager::runDockerContainer() {
    if (m_modelPath.isEmpty()) {
        emit errorOccurred(
            QStringLiteral(
                "Cannot start llama.cpp without a model."
            )
        );

        return;
    }

    const QFileInfo modelInfo(
        m_modelPath
    );

    if (
        !modelInfo.exists() ||
        !modelInfo.isFile() ||
        !modelInfo.isReadable()
    ) {
        emit errorOccurred(
            QStringLiteral(
                "The configured llama.cpp model is not available: %1"
            ).arg(
                m_modelPath
            )
        );

        return;
    }

    if (m_dockerProcess) {
        m_dockerProcess->disconnect(
            this
        );

        m_dockerProcess->deleteLater();

        m_dockerProcess =
                nullptr;
    }

    m_dockerStdout.clear();
    m_dockerStderr.clear();

    m_dockerProcess =
            new QProcess(
                this
            );

    connect(
        m_dockerProcess,
        &QProcess::finished,
        this,
        &LlamaManager::onDockerRunFinished
    );

    connect(
        m_dockerProcess,
        &QProcess::readyReadStandardOutput,
        this,
        &LlamaManager::onDockerOutputReady
    );

    connect(
        m_dockerProcess,
        &QProcess::readyReadStandardError,
        this,
        &LlamaManager::onDockerErrorReady
    );

    const QStringList args =
            dockerRunArguments();

    qDebug()
            << "[LLAMA] Starting container:"
            << dockerImage()
            << "model="
            << m_modelPath;

    qDebug()
            << "[LLAMA] docker run arguments:"
            << args;

    m_dockerProcess->start(
        QStringLiteral(
            "docker"
        ),
        args
    );

    if (
        !m_dockerProcess->waitForStarted(
            3000
        )
    ) {
        emit errorOccurred(
            QStringLiteral(
                "Failed to launch the docker process: %1"
            ).arg(
                m_dockerProcess->errorString()
            )
        );

        return;
    }
}


void LlamaManager::onDockerRunFinished(
    int exitCode,
    QProcess::ExitStatus status
) {
    if (m_dockerProcess) {
        const QByteArray remainingStdout =
                m_dockerProcess
                ->readAllStandardOutput();

        const QByteArray remainingStderr =
                m_dockerProcess
                ->readAllStandardError();

        if (!remainingStdout.isEmpty())
            m_dockerStdout +=
                    remainingStdout;

        if (!remainingStderr.isEmpty())
            m_dockerStderr +=
                    remainingStderr;
    }

    if (
        exitCode != 0 ||
        status != QProcess::NormalExit
    ) {
        QString error =
                QStringLiteral(
                    "Failed to start llama.cpp Docker container."
                );

        const QString stderrOutput =
                QString::fromUtf8(
                    m_dockerStderr
                ).trimmed();

        if (!stderrOutput.isEmpty()) {
            error +=
                    QStringLiteral(
                        ": "
                    ) +
                    stderrOutput;
        }

        emit errorOccurred(
            error
        );

        emit statusChanged();

        return;
    }

    const QString output =
            QString::fromUtf8(
                m_dockerStdout
            ).trimmed();

    qDebug()
            << "[LLAMA] docker run stdout:"
            << output;

    if (output.isEmpty()) {
        emit errorOccurred(
            QStringLiteral(
                "llama.cpp container started but no container ID was returned."
            )
        );

        emit statusChanged();

        return;
    }

    const QStringList tokens =
            output.split(
                QRegularExpression(
                    QStringLiteral(
                        "\\s+"
                    )
                ),
                Qt::SkipEmptyParts
            );

    if (tokens.isEmpty()) {
        emit errorOccurred(
            QStringLiteral(
                "Unable to determine the llama.cpp container ID."
            )
        );

        emit statusChanged();

        return;
    }

    QString candidateId =
            tokens.first().trimmed();

    const QRegularExpression idRegex(
        QStringLiteral(
            R"(^[0-9a-fA-F]{12,64}$)"
        )
    );

    bool foundValidId =
            idRegex
            .match(
                candidateId
            )
            .hasMatch();

    if (!foundValidId) {
        for (const QString &token: tokens) {
            if (
                idRegex
                .match(
                    token
                )
                .hasMatch()
            ) {
                candidateId =
                        token.trimmed();

                foundValidId =
                        true;

                break;
            }
        }
    }

    if (!foundValidId) {
        emit errorOccurred(
            QStringLiteral(
                "Docker returned unexpected output while starting llama.cpp: %1"
            ).arg(
                output
            )
        );

        emit statusChanged();

        return;
    }

    m_containerId =
            candidateId;

    m_containerStarted =
            true;

    m_ready =
            false;

    m_healthAttempts =
            0;

    qDebug()
            << "[LLAMA] Container started:"
            << m_containerId;

    qDebug()
            << "[LLAMA] Waiting for server:"
            << m_serverUrl;

    m_healthCheckTimer->start();

    emit statusChanged();
}


void LlamaManager::onDockerOutputReady() {
    if (!m_dockerProcess)
        return;

    const QByteArray data =
            m_dockerProcess
            ->readAllStandardOutput();

    if (data.isEmpty())
        return;

    m_dockerStdout +=
            data;

    qDebug()
            << "[LLAMA Docker stdout]"
            << data;
}


void LlamaManager::onDockerErrorReady() {
    if (!m_dockerProcess)
        return;

    const QByteArray data =
            m_dockerProcess
            ->readAllStandardError();

    if (data.isEmpty())
        return;

    m_dockerStderr +=
            data;

    qWarning()
            << "[LLAMA Docker stderr]"
            << data;
}


void LlamaManager::checkServerHealth() {
    if (!m_containerStarted) {
        m_healthCheckTimer->stop();

        return;
    }

    if (m_serverUrl.isEmpty()) {
        m_serverUrl =
                QUrl(
                    QStringLiteral(
                        "http://127.0.0.1:%1"
                    ).arg(
                        HOST_PORT
                    )
                );
    }

    const QUrl healthUrl =
            m_serverUrl.resolved(
                QUrl(
                    QStringLiteral(
                        "/health"
                    )
                )
            );

    QNetworkRequest request{
        healthUrl
    };

    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral(
            "TalosApp/1.0"
        )
    );

    QNetworkReply *reply =
            m_networkManager->get(
                request
            );

    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, reply]() {
            if (!reply)
                return;

            if (!m_containerStarted) {
                reply->deleteLater();

                return;
            }

            if (
                reply->error() ==
                QNetworkReply::NoError
            ) {
                const int status =
                        reply->attribute(
                            QNetworkRequest::
                            HttpStatusCodeAttribute
                        ).toInt();

                if (status == 200) {
                    m_healthCheckTimer->stop();

                    if (!m_ready) {
                        m_ready =
                                true;

                        qDebug()
                                << "[LLAMA] Server ready:"
                                << m_serverUrl;

                        emit serverReady();
                        emit statusChanged();
                    }

                    reply->deleteLater();

                    return;
                }
            }

            ++m_healthAttempts;

            /*
             * 2 seconds × 300 attempts = 10 minutes.
             *
             * A large model with a huge context configuration can
             * legitimately take substantial time to initialize.
             */
            if (
                m_healthAttempts <
                MAX_HEALTH_ATTEMPTS
            ) {
                reply->deleteLater();

                return;
            }

            m_healthCheckTimer->stop();

            QString error =
                    QStringLiteral(
                        "llama.cpp server did not become ready within timeout."
                    );

            if (
                reply->error() !=
                QNetworkReply::NoError
            ) {
                error +=
                        QStringLiteral(
                            " Network error: %1"
                        ).arg(
                            reply->errorString()
                        );
            } else {
                const int status =
                        reply->attribute(
                            QNetworkRequest::
                            HttpStatusCodeAttribute
                        ).toInt();

                error +=
                        QStringLiteral(
                            " HTTP status: %1"
                        ).arg(
                            status
                        );
            }

            emit errorOccurred(
                error
            );

            emit statusChanged();

            reply->deleteLater();
        }
    );
}


void LlamaManager::stopDockerContainer() {
    m_healthCheckTimer->stop();

    if (
        m_dockerProcess &&
        m_dockerProcess->state() !=
        QProcess::NotRunning
    ) {
        m_dockerProcess->disconnect(
            this
        );

        m_dockerProcess->kill();

        if (
            !m_dockerProcess
            ->waitForFinished(
                1000
            )
        ) {
            m_dockerProcess->terminate();

            m_dockerProcess
                    ->waitForFinished(
                        1000
                    );
        }
    }

    if (m_containerId.isEmpty()) {
        m_containerStarted =
                false;

        m_ready =
                false;

        m_dockerStdout.clear();
        m_dockerStderr.clear();

        return;
    }

    QProcess stop;

    stop.start(
        QStringLiteral(
            "docker"
        ),
        QStringList()
        << QStringLiteral(
            "stop"
        )
        << m_containerId
    );

    if (!stop.waitForFinished(5000)) {
        qWarning()
                << "[LLAMA] Docker stop timed out:"
                << m_containerId;

        stop.kill();
        stop.waitForFinished(1000);
    } else if (
        stop.exitCode() != 0
    ) {
        qWarning()
                << "[LLAMA] Failed to stop container:"
                << stop.readAllStandardError();
    } else {
        qDebug()
                << "[LLAMA] Container stopped:"
                << m_containerId;
    }

    m_containerStarted =
            false;

    m_ready =
            false;

    m_containerId.clear();

    m_dockerStdout.clear();
    m_dockerStderr.clear();

    emit statusChanged();
}

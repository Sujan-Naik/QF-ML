#include "../../include/inference/LlmClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QUrl>
#include <QDebug>

LlmClient::LlmClient(
    QNetworkAccessManager *networkManager,
    QObject *parent
)
    : QObject(parent)
      , m_networkManager(networkManager) {
}

void LlmClient::sendRequest(
    const Request &request
) {
    abortRequest();

    if (!m_networkManager) {
        emit requestError(
            QStringLiteral(
                "LLM network manager is unavailable."
            )
        );
        return;
    }

    QUrl url(request.url);

    if (url.scheme().isEmpty())
        url.setScheme(QStringLiteral("http"));

    if (!url.isValid()) {
        emit requestError(
            QStringLiteral(
                "Invalid LLM URL: %1"
            ).arg(
                url.errorString()
            )
        );
        return;
    }

    QNetworkRequest networkRequest(url);

    networkRequest.setHeader(
        QNetworkRequest::ContentTypeHeader,
        QStringLiteral("application/json")
    );

    networkRequest.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("TalosApp/1.0")
    );

    networkRequest.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy
    );

    networkRequest.setTransferTimeout(
        request.timeoutMs
    );

    QJsonObject body;

    body.insert(
        QStringLiteral("model"),
        request.model
    );

    body.insert(
        QStringLiteral("messages"),
        request.messages
    );

    body.insert(
        QStringLiteral("stream"),
        true
    );

    body.insert(
        QStringLiteral("temperature"),
        request.temperature
    );

    if (!request.grammar.isEmpty()) {
        body.insert(
            QStringLiteral("grammar"),
            request.grammar
        );
    }

    m_streamBuffer.clear();
    m_requestFailed = false;
    m_abortRequested = false;

    m_currentReply =
        m_networkManager->post(
            networkRequest,
            QJsonDocument(body).toJson(
                QJsonDocument::Compact
            )
        );

    connect(
        m_currentReply,
        &QNetworkReply::readyRead,
        this,
        &LlmClient::onReadyRead
    );

    connect(
        m_currentReply,
        &QNetworkReply::finished,
        this,
        &LlmClient::onFinished
    );

    connect(
        m_currentReply,
        QOverload<QNetworkReply::NetworkError>::of(
            &QNetworkReply::errorOccurred
        ),
        this,
        &LlmClient::onError
    );
}

void LlmClient::abortRequest() {
    if (!m_currentReply)
        return;

    m_abortRequested = true;

    QNetworkReply *reply =
            m_currentReply;

    m_currentReply = nullptr;

    reply->abort();
    reply->deleteLater();

    m_streamBuffer.clear();
}

bool LlmClient::isActive() const {
    return m_currentReply != nullptr;
}

void LlmClient::onReadyRead() {
    if (!m_currentReply ||
        m_requestFailed ||
        m_abortRequested) {
        return;
    }

    m_streamBuffer.append(
        m_currentReply->readAll()
    );

    while (
        m_streamBuffer.contains('\n')
    ) {
        const int newlineIndex =
                m_streamBuffer.indexOf('\n');

        QByteArray line =
                m_streamBuffer
                .left(newlineIndex);

        m_streamBuffer.remove(
            0,
            newlineIndex + 1
        );

        line =
                line.trimmed();

        if (line.isEmpty())
            continue;

        processLine(line);
    }
}

void LlmClient::processLine(
    const QByteArray &rawLine
) {
    QByteArray line =
            rawLine.trimmed();

    if (line.startsWith("data:"))
        line = line.mid(5).trimmed();

    if (line.isEmpty())
        return;

    if (line == "[DONE]") {
        return;
    }

    QJsonParseError parseError;

    const QJsonDocument document =
            QJsonDocument::fromJson(
                line,
                &parseError
            );

    if (
        parseError.error !=
        QJsonParseError::NoError
    ) {
        qWarning()
                << "[LLM] Failed to parse stream JSON:"
                << parseError.errorString()
                << line;

        return;
    }

    if (!document.isObject())
        return;

    const QJsonObject root =
            document.object();

    const QJsonValue choicesValue =
            root.value(
                QStringLiteral("choices")
            );

    if (!choicesValue.isArray())
        return;

    const QJsonArray choices =
            choicesValue.toArray();

    if (choices.isEmpty())
        return;

    const QJsonObject choice =
            choices.first().toObject();

    const QJsonObject delta =
            choice.value(
                QStringLiteral("delta")
            ).toObject();

    const QString content =
            delta.value(
                QStringLiteral("content")
            ).toString();

    if (!content.isEmpty())
        emit deltaReceived(content);
}

void LlmClient::onFinished() {
    QNetworkReply *reply =
            m_currentReply;

    if (!reply)
        return;

    m_currentReply = nullptr;

    if (!m_abortRequested &&
        !m_requestFailed &&
        reply->error() ==
        QNetworkReply::NoError) {
        if (!m_streamBuffer.isEmpty()) {
            const QByteArray remaining =
                    m_streamBuffer.trimmed();

            if (!remaining.isEmpty())
                processLine(remaining);
        }

        emit requestFinished();
    }

    reply->deleteLater();

    m_streamBuffer.clear();
    m_abortRequested = false;
}

void LlmClient::onError(
    QNetworkReply::NetworkError error
) {
    if (m_abortRequested)
        return;

    m_requestFailed = true;

    QString message =
            QStringLiteral(
                "LLM network error (%1)"
            ).arg(
                static_cast<int>(error)
            );

    if (m_currentReply) {
        const QString errorString =
                m_currentReply->errorString();

        if (!errorString.isEmpty()) {
            message +=
                    QStringLiteral(": ")
                    + errorString;
        }
    }

    emit requestError(message);
}

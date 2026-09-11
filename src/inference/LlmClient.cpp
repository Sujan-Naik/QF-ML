#include "../../include/inference/LlmClient.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

LlmClient::LlmClient(
    QNetworkAccessManager *networkManager,
    QObject *parent)
    : QObject(parent)
    , m_networkManager(networkManager)
{
}

void LlmClient::sendRequest(
    const Request &request)
{
    abortRequest();

    if (!m_networkManager) {
        emit requestError(
            QStringLiteral(
                "LLM network manager is unavailable."));
        return;
    }

    QUrl url(
        request.url);

    if (url.scheme().isEmpty()) {
        url.setScheme(
            QStringLiteral("http"));
    }

    if (!url.isValid()) {
        emit requestError(
            QStringLiteral(
                "Invalid LLM URL: %1")
            .arg(
                url.errorString()));
        return;
    }

    QNetworkRequest networkRequest(
        url);

    networkRequest.setHeader(
        QNetworkRequest::ContentTypeHeader,
        QStringLiteral("application/json"));

    networkRequest.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("TalosApp/1.0"));

    networkRequest.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);

    networkRequest.setTransferTimeout(
        request.timeoutMs);

    QJsonObject body;

    body.insert(
        QStringLiteral("model"),
        request.model);

    body.insert(
        QStringLiteral("messages"),
        request.messages);

    body.insert(
        QStringLiteral("stream"),
        true);

    body.insert(
        QStringLiteral("temperature"),
        request.temperature);

    if (!request.grammar.isEmpty()) {
        body.insert(
            QStringLiteral("grammar"),
            request.grammar);
    }

    m_streamBuffer.clear();
    m_currentSseData.clear();
    m_requestFailed = false;
    m_abortRequested = false;

    m_currentReply =
        m_networkManager->post(
            networkRequest,
            QJsonDocument(body).toJson(
                QJsonDocument::Compact));

    QNetworkReply *reply =
        m_currentReply;

    connect(
        reply,
        &QNetworkReply::readyRead,
        this,
        &LlmClient::onReadyRead);

    connect(
        reply,
        &QNetworkReply::finished,
        this,
        &LlmClient::onFinished);

    connect(
        reply,
        QOverload<QNetworkReply::NetworkError>::of(
            &QNetworkReply::errorOccurred),
        this,
        &LlmClient::onError);

    qDebug()
        << "[LlmClient] POST started"
        << "reply="
        << reply;
}

void LlmClient::abortRequest()
{
    QNetworkReply *reply =
        m_currentReply;

    if (!reply) {
        return;
    }

    m_currentReply =
        nullptr;

    m_abortRequested =
        true;

    reply->abort();
    reply->deleteLater();

    m_streamBuffer.clear();
    m_currentSseData.clear();

    qDebug()
        << "[LlmClient] Aborted reply="
        << reply;
}

bool LlmClient::isActive() const
{
    return m_currentReply != nullptr;
}

void LlmClient::onReadyRead()
{
    auto *reply =
        qobject_cast<QNetworkReply *>(
            sender());

    if (!reply ||
        reply != m_currentReply ||
        m_requestFailed ||
        m_abortRequested) {
        return;
    }

    const QByteArray data =
        reply->readAll();

    if (data.isEmpty()) {
        return;
    }

    m_streamBuffer.append(
        data);

    consumeStreamBuffer();
}

void LlmClient::consumeStreamBuffer()
{
    while (true) {
        int lineEndIndex =
            m_streamBuffer.indexOf('\n');

        if (lineEndIndex == -1) {
            break;
        }

        int lineLength =
            lineEndIndex;

        if (lineLength > 0 &&
            m_streamBuffer.at(lineLength - 1) == '\r') {
            lineLength--;
        }

        QByteArray line =
            m_streamBuffer.left(lineLength);

        m_streamBuffer.remove(
            0,
            lineEndIndex + 1);

        processSseLine(line);
    }
}

void LlmClient::processSseLine(
    const QByteArray &line)
{
    if (line.isEmpty()) {
        if (!m_currentSseData.isEmpty()) {
            dispatchSseMessage(
                m_currentSseData);

            m_currentSseData.clear();
        }

        return;
    }

    if (line.startsWith(':')) {
        return;
    }

    int colonIndex =
        line.indexOf(':');

    QByteArray fieldName;
    QByteArray fieldValue;

    if (colonIndex != -1) {
        fieldName =
            line.left(colonIndex);

        fieldValue =
            line.mid(colonIndex + 1);

        if (fieldValue.startsWith(' ')) {
            fieldValue.remove(0, 1);
        }
    } else {
        fieldName =
            line;
    }

    if (fieldName == "data") {
        if (!m_currentSseData.isEmpty()) {
            m_currentSseData.append('\n');
        }

        m_currentSseData.append(
            fieldValue);
    }
}

void LlmClient::dispatchSseMessage(
    const QByteArray &rawPayload)
{
    QByteArray payload =
        rawPayload.trimmed();

    if (payload.isEmpty()) {
        return;
    }

    if (payload == "[DONE]") {
        return;
    }

    QJsonParseError parseError;

    const QJsonDocument document =
        QJsonDocument::fromJson(
            payload,
            &parseError);

    if (parseError.error !=
        QJsonParseError::NoError) {

        qWarning()
            << "[LLM] Failed to parse SSE JSON payload:"
            << parseError.errorString()
            << payload;

        return;
    }

    if (!document.isObject()) {
        return;
    }

    const QJsonObject root =
        document.object();

    const QJsonValue choicesValue =
        root.value(
            QStringLiteral("choices"));

    if (!choicesValue.isArray()) {
        return;
    }

    const QJsonArray choices =
        choicesValue.toArray();

    if (choices.isEmpty()) {
        return;
    }

    const QJsonObject choice =
        choices.first().toObject();

    QJsonObject delta =
        choice.value(
            QStringLiteral("delta"))
            .toObject();

    if (delta.isEmpty()) {
        delta =
            choice.value(
                QStringLiteral("message"))
                .toObject();
    }

    const QString content =
        delta.value(
            QStringLiteral("content"))
            .toString();

    if (!content.isEmpty()) {
        emit deltaReceived(
            content);
    }
}

void LlmClient::onFinished()
{
    auto *reply =
        qobject_cast<QNetworkReply *>(
            sender());

    if (!reply) {
        return;
    }

    if (reply != m_currentReply) {
        qDebug()
            << "[LlmClient] Ignoring stale finished reply="
            << reply;

        reply->deleteLater();

        return;
    }

    m_currentReply =
        nullptr;

    const bool aborted =
        m_abortRequested;

    const bool failed =
        m_requestFailed;

    const QNetworkReply::NetworkError error =
        reply->error();

    qDebug()
        << "[LlmClient] Finished reply="
        << reply
        << "aborted="
        << aborted
        << "failed="
        << failed
        << "error="
        << error;

    if (!aborted &&
        !failed &&
        error ==
            QNetworkReply::NoError) {

        consumeStreamBuffer();

        if (!m_streamBuffer.isEmpty()) {
            QByteArray remaining =
                m_streamBuffer;

            m_streamBuffer.clear();

            if (remaining.endsWith('\r')) {
                remaining.chop(1);
            }

            processSseLine(
                remaining);
        }

        if (!m_currentSseData.isEmpty()) {
            dispatchSseMessage(
                m_currentSseData);

            m_currentSseData.clear();
        }

        emit requestFinished();
    }

    reply->deleteLater();

    m_streamBuffer.clear();
    m_currentSseData.clear();
    m_abortRequested =
        false;
    m_requestFailed =
        false;
}

void LlmClient::onError(
    QNetworkReply::NetworkError error)
{
    auto *reply =
        qobject_cast<QNetworkReply *>(
            sender());

    if (!reply) {
        return;
    }

    if (reply != m_currentReply) {
        qDebug()
            << "[LlmClient] Ignoring stale error reply="
            << reply;

        return;
    }

    if (m_abortRequested) {
        return;
    }

    m_requestFailed =
        true;

    QString message =
        QStringLiteral(
            "LLM network error (%1)")
        .arg(
            static_cast<int>(
                error));

    const QString errorString =
        reply->errorString();

    if (!errorString.isEmpty()) {
        message +=
            QStringLiteral(": ") +
            errorString;
    }

    emit requestError(
        message);
}
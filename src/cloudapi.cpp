/*
    Copyright (C) 2016-19 Sebastian J. Wolf

    This file is part of Wunderfitz.

    Wunderfitz is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Wunderfitz is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Wunderfitz. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cloudapi.h"
#include "curiosity.h"

CloudApi::CloudApi(QNetworkAccessManager *manager, Curiosity *parent)
    : QObject(parent)
    , curiosity(parent)
{
    this->networkAccessManager = manager;
}

QString CloudApi::stripTrailingSlashes(const QString &path)
{
    QString strippedPath = path;
    while (strippedPath.endsWith('/')) {
        strippedPath.chop(1);
    }
    return strippedPath;
}

void CloudApi::opticalCharacterRecognition(const QString &imagePath, const QString &sourceLanguage)
{
    qDebug() << "CloudApi::opticalCharacterRecognition" << imagePath << sourceLanguage;

    QUrl url = QUrl(curiosity->getComputerVisionEndpoint().trimmed());
    url.setPath(stripTrailingSlashes(url.path()) + API_COMPUTER_VISION_ENDPOINT_PATH);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("api-version", "2024-02-01");
    urlQuery.addQueryItem("features", "read");
    if (sourceLanguage != "unk") {
        urlQuery.addQueryItem("language", sourceLanguage);
    }
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    request.setRawHeader(QByteArray("Ocp-Apim-Subscription-Key"), curiosity->getComputerVisionKey().toLocal8Bit());

    QFile *file = new QFile(imagePath);
    file->open(QIODevice::ReadOnly);
    QByteArray rawImage = file->readAll();
    file->close();
    file->deleteLater();

    QNetworkReply *reply = networkAccessManager->post(request, rawImage);
    reply->setObjectName(imagePath);

    connect(reply, SIGNAL(finished()), this, SLOT(handleOcrUploadFinished()));
    connect(reply, SIGNAL(uploadProgress(qint64,qint64)), this, SLOT(handleOcrUploadProgress(qint64,qint64)));
}

void CloudApi::translate(const QString &text, const QString &targetLanguage)
{
    qDebug() << "CloudApi::translate" << text << targetLanguage;

    QUrl url = QUrl(curiosity->getTranslatorTextEndpoint().trimmed());
    QString path = stripTrailingSlashes(url.path());
    // Custom domain endpoints (https://<name>.cognitiveservices.azure.com/) serve the Translator below
    // /translator/text/v3.0, the global endpoint serves it at the root
    if (url.host().endsWith(API_CUSTOM_DOMAIN_HOST_SUFFIX, Qt::CaseInsensitive)
            && !path.contains(API_TRANSLATOR_TEXT_CUSTOM_DOMAIN_PATH, Qt::CaseInsensitive)) {
        path += API_TRANSLATOR_TEXT_CUSTOM_DOMAIN_PATH;
    }
    if (!path.endsWith(API_TRANSLATOR_TEXT_ENDPOINT_PATH)) {
        path += API_TRANSLATOR_TEXT_ENDPOINT_PATH;
    }
    url.setPath(path);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("api-version", "3.0");
    urlQuery.addQueryItem("to", targetLanguage);
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader(QByteArray("Ocp-Apim-Subscription-Key"), curiosity->getTranslatorTextKey().toLocal8Bit());
    request.setRawHeader(QByteArray("Ocp-Apim-Subscription-Region"), curiosity->getTranslatorTextRegion().toLocal8Bit());

    QJsonObject jsonText;
    jsonText.insert("Text", text);
    QJsonArray jsonArray;
    jsonArray.append(jsonText);

    QJsonDocument requestDocument(jsonArray);
    qDebug() << requestDocument.toJson();
    QByteArray jsonAsByteArray = requestDocument.toJson();

    QNetworkReply *reply = networkAccessManager->post(request, jsonAsByteArray);

    connect(reply, SIGNAL(finished()), this, SLOT(handleTranslateFinished()));
}

void CloudApi::handleOcrUploadProgress(qint64 bytesSent, qint64 bytesTotal)
{
    qDebug() << "CloudApi::handleOcrUploadProgress" << bytesSent << bytesTotal;
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    emit ocrUploadStatus(reply->objectName(), bytesSent, bytesTotal);
}

QString CloudApi::getAzureErrorMessage(QNetworkReply *reply)
{
    QByteArray responseBody = reply->readAll();
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    qWarning() << "CloudApi::getAzureErrorMessage:" << reply->error() << statusCode << reply->errorString() << responseBody;

    QJsonDocument errorDocument = QJsonDocument::fromJson(responseBody);
    if (errorDocument.isObject()) {
        QJsonObject errorObject = errorDocument.object().value("error").toObject();
        QString message = errorObject.value("message").toString();
        if (!message.isEmpty()) {
            return QString("Azure (%1): %2").arg(statusCode).arg(message);
        }
    }
    if (statusCode != 0) {
        QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        QString body = QString::fromUtf8(responseBody).simplified().left(200);
        return QString("Azure (%1 %2) %3").arg(statusCode).arg(reason).arg(body).trimmed();
    }
    return reply->errorString();
}

void CloudApi::handleOcrUploadFinished()
{
    qDebug() << "CloudApi::handleOcrUploadFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isNull()) {
            // Azure closes the connection while the image is still uploading when it rejects the request
            // (e.g. an invalid key), so there is no response. An empty request gets the reason right away.
            qWarning() << "CloudApi::handleOcrUploadFinished: no response," << reply->errorString() << "- probing Azure for the reason";
            QNetworkReply *probeReply = networkAccessManager->post(reply->request(), QByteArray());
            probeReply->setObjectName(reply->objectName());
            probeReply->setProperty("originalError", reply->errorString());
            connect(probeReply, SIGNAL(finished()), this, SLOT(handleOcrProbeFinished()));
        } else {
            emit ocrUploadError(reply->objectName(), getAzureErrorMessage(reply));
        }
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        emit ocrUploadSuccessful(reply->objectName(), jsonDocument.object());
    } else {
        emit ocrUploadError(reply->objectName(), "Wunderfitz couldn't understand Azure's response!");
    }
}

void CloudApi::handleOcrProbeFinished()
{
    qDebug() << "CloudApi::handleOcrProbeFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // 400 only complains about the missing image, i.e. key and endpoint were accepted
    if (statusCode == 0 || statusCode == 400) {
        emit ocrUploadError(reply->objectName(), reply->property("originalError").toString());
    } else {
        emit ocrUploadError(reply->objectName(), getAzureErrorMessage(reply));
    }
}

void CloudApi::handleTranslateFinished()
{
    qDebug() << "CloudApi::handleTranslateFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        emit translateError(getAzureErrorMessage(reply));
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    qDebug() << jsonDocument.toJson();
    if (jsonDocument.isArray()) {
        emit translateSuccessful(jsonDocument.array());
    } else {
        emit translateError("Wunderfitz couldn't understand Azure's response!");
    }
}

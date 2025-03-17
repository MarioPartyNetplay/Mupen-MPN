#include "Common/HttpRequest.hpp"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QEventLoop>
#include <QByteArray>
#include <QHttpMultiPart>

namespace Common
{
class HttpRequest::Impl
{
public:
    enum class Method { GET, POST };
    explicit Impl(std::chrono::milliseconds timeout_ms, ProgressCallback callback);
    bool IsValid() const;
    std::string GetHeaderValue(std::string_view name) const;
    s32 GetLastResponseCode();
    Response Fetch(const std::string& url, Method method, const Headers& headers, const u8* payload,
                   size_t size, AllowedReturnCodes codes = AllowedReturnCodes::Ok_Only);

private:
    QNetworkAccessManager m_manager;
    QNetworkReply* m_reply = nullptr;
    Headers m_response_headers;
    int m_response_code = 0;
};

HttpRequest::Impl::Impl(std::chrono::milliseconds timeout_ms, ProgressCallback callback) {}

bool HttpRequest::Impl::IsValid() const { return true; }

s32 HttpRequest::Impl::GetLastResponseCode() { return m_response_code; }

HttpRequest::Response HttpRequest::Impl::Fetch(const std::string& url, Method method,
                                               const Headers& headers, const u8* payload,
                                               size_t size, AllowedReturnCodes codes)
{
    QNetworkRequest request(QUrl(QString::fromStdString(url)));
    for (const auto& [key, value] : headers)
    {
        request.setRawHeader(QByteArray::fromStdString(key), QByteArray::fromStdString(value.value_or("")));
    }

    QEventLoop loop;
    if (method == Method::GET)
        m_reply = m_manager.get(request);
    else
    {
        QByteArray postData(reinterpret_cast<const char*>(payload), static_cast<int>(size));
        m_reply = m_manager.post(request, postData);
    }

    QObject::connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    HttpRequest::Response response;
    if (m_reply->error() == QNetworkReply::NoError)
    {
        response = m_reply->readAll().toStdString();
    }
    m_response_code = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_reply->deleteLater();

    return response;
}
} // namespace Common
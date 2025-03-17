#ifndef COMMON_HTTPREQUEST_HPP
#define COMMON_HTTPREQUEST_HPP

#include <chrono>
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QEventLoop>
#include <QByteArray>
#include <QHttpMultiPart>

namespace Common
{
class HttpRequest
{
public:
    using Headers = std::unordered_map<std::string, std::optional<std::string>>;
    using Response = std::string;
    using ProgressCallback = std::function<bool(s64, s64, s64, s64)>;
    
    explicit HttpRequest(std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(3000),
                         ProgressCallback callback = nullptr);
    ~HttpRequest();
    
    bool IsValid() const;
    void FollowRedirects(long max);
    s32 GetLastResponseCode() const;
    std::string GetHeaderValue(std::string_view name) const;
    Response Get(const std::string& url, const Headers& headers = {});
    Response Post(const std::string& url, const std::vector<u8>& payload, const Headers& headers = {});
    Response Post(const std::string& url, const std::string& payload, const Headers& headers = {});
    
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

class HttpRequest::Impl
{
public:
    enum class Method { GET, POST };
    explicit Impl(std::chrono::milliseconds timeout_ms, ProgressCallback callback);
    bool IsValid() const;
    std::string GetHeaderValue(std::string_view name) const;
    s32 GetLastResponseCode();
    Response Fetch(const std::string& url, Method method, const Headers& headers, const u8* payload,
                   size_t size);

private:
    QNetworkAccessManager m_manager;
    QNetworkReply* m_reply = nullptr;
    Headers m_response_headers;
    int m_response_code = 0;
};

} // namespace Common

#endif // COMMON_HTTPREQUEST_HPP

#include "cap/Http.hpp"

#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace cap {
namespace {

size_t write_callback(void* pointer, size_t size, size_t count, void* userdata)
{
  auto* body = static_cast<std::string*>(userdata);
  body->append(static_cast<char*>(pointer), size * count);

  return size * count;
}

} // namespace

std::string url_encode(CURL* curl, const std::string& value)
{
  char* encoded = curl_easy_escape(
    curl,
    value.c_str(),
    static_cast<int>(value.size())
  );

  if(!encoded) {
    return value;
  }

  std::string result = encoded;
  curl_free(encoded);

  return result;
}

HttpResult http_get(const std::string& url, long timeout)
{
  CURL* curl = curl_easy_init();

  if(!curl) {
    throw std::runtime_error("curl init failed");
  }

  char error_buffer[CURL_ERROR_SIZE];
  error_buffer[0] = '\0';

  HttpResult result;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);

  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cap-offchain-etl/1.0");
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

  CURLcode rc = curl_easy_perform(curl);

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);

  char* content_type = nullptr;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);

  if(content_type) {
    result.content_type = content_type;
  }

  if(rc != CURLE_OK) {
    std::string curl_error = error_buffer[0]
      ? std::string(error_buffer)
      : std::string(curl_easy_strerror(rc));

    curl_easy_cleanup(curl);

    throw std::runtime_error(
      curl_error +
      " url=" + url +
      " curl_code=" + std::to_string(static_cast<int>(rc))
    );
  }

  curl_easy_cleanup(curl);

  return result;
}

void http_download_file(
  const std::string& url,
  const std::string& path,
  long timeout
)
{
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());

  FILE* file = fopen(path.c_str(), "wb");

  if(!file) {
    throw std::runtime_error("cannot create download file: " + path);
  }

  CURL* curl = curl_easy_init();

  if(!curl) {
    fclose(file);
    throw std::runtime_error("curl init failed");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cap-offchain-etl/1.0");

  CURLcode rc = curl_easy_perform(curl);

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  curl_easy_cleanup(curl);
  fclose(file);

  if(rc != CURLE_OK) {
    std::filesystem::remove(path);
    throw std::runtime_error(std::string(curl_easy_strerror(rc)) + " for " + url);
  }

  if(status >= 400) {
    std::filesystem::remove(path);
    throw std::runtime_error("HTTP " + std::to_string(status) + " for " + url);
  }
}

} // namespace cap

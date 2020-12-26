#include "webdav_utils.h"

#include <coro/http/http_parse.h>

#include <cstring>
#include <sstream>

namespace coro::cloudstorage::util {

namespace {

const char *kDayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string GetRFC1123(int64_t timestamp) {
  const int kBufferSize = 29;
  tm tm = coro::http::gmtime(timestamp);
  std::string buffer(kBufferSize, 0);
  strftime(buffer.data(), kBufferSize + 1, "---, %d --- %Y %H:%M:%S GMT", &tm);
  memcpy(buffer.data(), kDayNames[tm.tm_wday], 3);
  memcpy(buffer.data() + 8, kMonthNames[tm.tm_mon], 3);
  return buffer;
}

}  // namespace

std::string GetMultiStatusResponse(std::span<const std::string> responses) {
  std::stringstream stream;
  stream
      << R"(<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">)";
  for (const std::string &response : responses) {
    stream << response;
  }
  stream << "</d:multistatus>";
  return stream.str();
}

std::string GetElement(const ElementData &data) {
  std::stringstream stream;
  stream << "<d:response><d:href>"
         << http::EncodeUriPath(data.path + (data.is_directory &&
                                                     !data.path.empty() &&
                                                     data.path.back() != '/'
                                                 ? "/"
                                                 : ""))
         << "</d:href>"
         << "<d:propstat><d:status>HTTP/1.1 200 OK</d:status>"
         << "<d:prop>"
         << "<d:displayname>" << http::EncodeUri(data.name)
         << "</d:displayname>";
  if (data.size) {
    stream << "<d:getcontentlength>" << *data.size << "</d:getcontentlength>";
  }
  if (data.mime_type) {
    stream << "<d:getcontenttype>" << *data.mime_type << "</d:getcontenttype>";
  }
  if (data.timestamp) {
    stream << "<d:getlastmodified>" << GetRFC1123(*data.timestamp)
           << "</d:getlastmodified>";
  }
  stream << "<d:resourcetype>" << (data.is_directory ? "<d:collection/>" : "")
         << "</d:resourcetype>"
         << "</d:prop></d:propstat></d:response>";
  return stream.str();
}

}  // namespace coro::cloudstorage::util
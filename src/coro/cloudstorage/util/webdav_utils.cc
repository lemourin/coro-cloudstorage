#include "webdav_utils.h"

#include <sstream>

namespace coro::cloudstorage::util {

std::string GetMultiStatusResponse(std::span<const std::string> responses) {
  std::stringstream stream;
  stream
      << R"(<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">)";
  for (const std::string& response : responses) {
    stream << response;
  }
  stream << "</d:multistatus>";
  return stream.str();
}

std::string GetElement(const ElementData& data) {
  std::stringstream stream;
  stream << "<d:response><d:href>"
         << data.path + (data.is_directory && !data.path.empty() &&
                                 data.path.back() != '/'
                             ? "/"
                             : "")
         << "</d:href>"
         << "<d:propstat><d:status>HTTP/1.1 200 OK</d:status>"
         << "<d:prop>"
         << "<d:displayname>" << data.name << "</d:displayname>";
  if (data.size) {
    stream << "<d:getcontentlength>" << *data.size << "</d:getcontentlength>";
  }
  if (data.mime_type) {
    stream << "<d:getcontenttype>" << *data.mime_type << "</d:getcontenttype>";
  }
  stream << "<d:resourcetype>" << (data.is_directory ? "<d:collection/>" : "")
         << "</d:resourcetype>"
         << "</d:prop></d:propstat></d:response>";
  return stream.str();
}

}  // namespace coro::cloudstorage::util
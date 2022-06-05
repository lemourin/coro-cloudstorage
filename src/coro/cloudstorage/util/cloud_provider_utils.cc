#include "coro/cloudstorage/util/cloud_provider_utils.h"

namespace coro::cloudstorage::util {

FileType GetFileType(std::string_view mime_type) {
  if (mime_type.find("audio") == 0) {
    return FileType::kAudio;
  } else if (mime_type.find("image") == 0) {
    return FileType::kImage;
  } else if (mime_type.find("video") == 0) {
    return FileType::kVideo;
  } else {
    return FileType::kUnknown;
  }
}

}  // namespace coro::cloudstorage::util
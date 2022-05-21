#ifndef CORO_CLOUDSTORAGE_UTIL_PROVIDERS_H
#define CORO_CLOUDSTORAGE_UTIL_PROVIDERS_H

#include "coro/util/type_list.h"

#ifdef NDEBUG
#include "coro/cloudstorage/providers/amazon_s3.h"
#include "coro/cloudstorage/providers/box.h"
#include "coro/cloudstorage/providers/dropbox.h"
#include "coro/cloudstorage/providers/google_drive.h"
#include "coro/cloudstorage/providers/hubic.h"
#include "coro/cloudstorage/providers/local_filesystem.h"
#include "coro/cloudstorage/providers/mega.h"
#include "coro/cloudstorage/providers/one_drive.h"
#include "coro/cloudstorage/providers/pcloud.h"
#include "coro/cloudstorage/providers/webdav.h"
#include "coro/cloudstorage/providers/yandex_disk.h"
#else
#include "coro/cloudstorage/providers/dropbox.h"
#endif

namespace coro::cloudstorage::util {

#ifdef NDEBUG
using CloudProviderTypeList =
    coro::util::TypeList<GoogleDrive, Mega, AmazonS3, Box, Dropbox, OneDrive,
                         PCloud, WebDAV, YandexDisk, HubiC, LocalFileSystem>;
#else
using CloudProviderTypeList = coro::util::TypeList<Dropbox>;
#endif

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_PROVIDERS_H

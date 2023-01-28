# coro-cloudstorage

### vcpkg

```bash
vcpkg install \
  curl \
  libevent \
  boost-filesystem \
  boost-regex \
  nlohmann-json \
  ffmpeg[zlib] \
  fmt \
  cryptopp \
  c-ares \
  pugixml

mkdir build
cd build

cmake .. \
  -DCMAKE_TOOLCHAIN_FILE="$(which vcpkg)/../scripts/buildsystems/vcpkg.cmake"

cmake --build .
```

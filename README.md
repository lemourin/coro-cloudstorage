# coro-cloudstorage

### vcpkg

```bash
mkdir build
cd build

cmake .. \
  -DCMAKE_TOOLCHAIN_FILE="$(which vcpkg)/../scripts/buildsystems/vcpkg.cmake"

cmake --build .
```

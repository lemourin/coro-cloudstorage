#!/usr/bin/env bash

set -e

cd $(dirname $0)

git submodule update --init --checkout --recursive -f

curl https://raw.githubusercontent.com/noloader/cryptopp-cmake/CRYPTOPP_8_2_0/CMakeLists.txt \
  -s -o contrib/cryptopp/CMakeLists.txt
sed -i 's/\\\"winapifamily\.h\\\"/winapifamily\.h/g' contrib/cryptopp/CMakeLists.txt 
curl https://raw.githubusercontent.com/noloader/cryptopp-cmake/master/cryptopp-config.cmake \
  -s -o contrib/cryptopp/cryptopp-config.cmake

./contrib/coro-http/bootstrap.sh

git checkout contrib/megasdk
cd contrib/megasdk
git apply ../patches/mega-fix-build.patch

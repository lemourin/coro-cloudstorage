#!/usr/bin/env bash

set -e

cd $(dirname $0)

git submodule update --init --checkout --recursive -f

./contrib/coro-http/bootstrap.sh


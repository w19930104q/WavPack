#!/bin/sh
# run_flac_tests.sh -- generate the fixed FLAC sample set and run the
# transcoding acceptance tests against freshly built wavpack/wvunpack.
#
# Binary locations can be overridden with the WAVPACK, WVUNPACK and WVTAG
# environment variables; the default is a CMake build in ../../build.

set -e

here=$(dirname "$0")
root="$here/../.."

WAVPACK=${WAVPACK:-"$root/build/wavpack"}
WVUNPACK=${WVUNPACK:-"$root/build/wvunpack"}
WVTAG=${WVTAG:-"$root/build/wvtag"}

workdir=$(mktemp -d "${TMPDIR:-/tmp}/flac-tests.XXXXXX")
trap 'rm -rf "$workdir"' EXIT

python3 -I "$here/flac_tests.py" run "$workdir" \
    --wavpack "$WAVPACK" --wvunpack "$WVUNPACK" --wvtag "$WVTAG"

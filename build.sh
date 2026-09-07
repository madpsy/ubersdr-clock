#!/usr/bin/env bash
#
# Build ubersdr-clock for amd64 and arm64, and check that each one decodes.
#
# The binaries get copied into the UberSDR container to run, so they are built
# inside ubuntu:24.04 — the same image the container's runtime stage uses.
# Building on the host instead works right up until the host is newer than the
# container: libstdc++ is the only dependency this binary has, and a binary
# built against a newer one dies at startup on a GLIBCXX_ version error rather
# than anything that names the real problem.
#
# arm64 is built by running an arm64 ubuntu:24.04 under binfmt/qemu, not by
# cross-compiling, so the toolchain is the target toolchain and CMake sees the
# target arch. Slow, and correct without a sysroot to keep in step.
#
# Building is the easy half. A binary that links and runs can still fail to
# decode anything, so unless told otherwise this feeds each one a synthetic
# WWV minute set from tools/wwvgen.py and checks it reaches a lock with the
# time it was given.
#
# Usage:
#   ./build.sh [options]
#
#   --arch LIST     comma-separated: amd64, arm64 (default: both)
#   --native        build on this host with the host toolchain instead of in a
#                   container.  This host arch only; for a quick edit-compile
#                   loop, not for anything you intend to ship
#   --clean         delete the build trees first
#   --no-check      build only, skip the decode check
#   --image IMAGE   build container image (default: ubuntu:24.04)
#   -j N            parallel jobs (default: all cores)
#

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
arches="amd64 arm64"
image=ubuntu:24.04
native=0
clean=0
check=1
jobs=$(nproc 2>/dev/null || echo 4)

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)     arches=$(echo "$2" | tr ',' ' '); shift 2 ;;
        --native)   native=1; shift ;;
        --clean)    clean=1; shift ;;
        --no-check) check=0; shift ;;
        --image)    image=$2; shift 2 ;;
        -j)         jobs=$2; shift 2 ;;
        -j*)        jobs=${1#-j}; shift ;;
        -h|--help)  sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^#\{0,1\} \{0,1\}//'; exit 0 ;;
        *)          echo "build.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

say()  { printf '\n== %s\n' "$*"; }
fail() { printf '\nbuild.sh: %s\n' "$*" >&2; exit 1; }

for a in $arches; do
    case "$a" in
        amd64|arm64|arm|386) ;;
        *) fail "unknown arch '$a' (expected amd64, arm64, arm or 386)" ;;
    esac
done

# --- The work done inside each container ---------------------------------
#
# Fed to bash on stdin rather than passed as `bash -c '...'`: an apostrophe
# anywhere in here — including in a comment — would close the quote and
# silently truncate the rest of the script, and the build would still exit 0.
#
# $1 = arch label, $2 = jobs, $3 = run the check, $4:$5 = host uid:gid
container_script=$(cat <<'CONTAINER_EOF'
set -euo pipefail
arch=$1; jobs=$2; check=$3; uid=$4; gid=$5

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    cmake ninja-build g++ python3 >/dev/null

build=/src/build-$arch
cmake -S /src -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build" -j "$jobs"

binary=$(ls "$build"/ubersdr-clock_* 2>/dev/null | head -1)
if [ ! -x "$binary" ]; then
    echo "no binary was produced" >&2
    exit 1
fi

if [ "$check" = 1 ]; then
    # Six minutes of synthetic WWV: two to anchor the frame, two more for the
    # voter to reach a lock. wwvgen prints the time it encoded on stderr.
    want=$(python3 /src/tools/wwvgen.py --minutes 6 2>&1 >/tmp/wwv.raw \
           | sed -n 's/.*from \([0-9T:-]*\)Z.*/\1/p')
    "$binary" --no-seconds --diag-seconds 0 < /tmp/wwv.raw > /tmp/out.jsonl

    if ! grep -q '"state":"locked"' /tmp/out.jsonl; then
        echo "built, but never reached a lock on a clean synthetic signal" >&2
        tail -3 /tmp/out.jsonl >&2
        exit 1
    fi
    got=$(grep '"type":"time"' /tmp/out.jsonl | tail -1)
    if [ -z "$got" ]; then
        echo "built and locked, but emitted no time event" >&2
        exit 1
    fi
    # The generator starts at $want and runs 6 minutes, so the last decode is
    # some minutes later; the date and hour are what must match.
    day=${want%T*}
    if ! printf %s "$got" | grep -q "\"utc\":\"$day"; then
        echo "decoded the wrong date: wanted $day, got:" >&2
        echo "$got" >&2
        exit 1
    fi
    quality=$(printf %s "$got" | sed -n 's/.*"quality":\([0-9]*\).*/\1/p')
    utc=$(printf %s "$got" | sed -n 's/.*"utc":"\([^"]*\)".*/\1/p')
    echo "CHECK_OK $utc quality=$quality"
fi

# The container runs as root; without this the build tree and the binary come
# out root-owned and the next non-root build cannot delete them.
chown -R "$uid:$gid" "$build"
CONTAINER_EOF
)

if [ "$clean" = 1 ]; then
    say "Removing build trees"
    rm -rf "$repo"/build-* "$repo"/build "$repo"/ubersdr-clock_*
fi

built=""

if [ "$native" = 1 ]; then
    say "Building natively (host toolchain)"
    cmake -S "$repo" -B "$repo/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$repo/build" -j "$jobs"
    binary=$(ls "$repo"/build/ubersdr-clock_* 2>/dev/null | head -1) \
        || fail "the build produced no binary"
    cp "$binary" "$repo/"
    built="$repo/$(basename "$binary")"
    if [ "$check" = 1 ]; then
        say "Checking $(basename "$binary")"
        python3 "$repo/tools/wwvgen.py" --minutes 6 2>/dev/null \
            | "$binary" --no-seconds --diag-seconds 0 > /tmp/ubersdr-clock-check.jsonl
        grep -q '"state":"locked"' /tmp/ubersdr-clock-check.jsonl \
            || fail "built, but never reached a lock on a clean synthetic signal"
        echo "  $(grep '"type":"time"' /tmp/ubersdr-clock-check.jsonl | tail -1 \
              | sed -n 's/.*"utc":"\([^"]*\)".*"quality":\([0-9]*\).*/\1 quality=\2/p')"
    fi
    say "Done"
    echo "  $built"
    exit 0
fi

command -v docker >/dev/null 2>&1 \
    || fail "docker is needed to build for both architectures; use --native to
build only for this host"

# arm64 on an amd64 host needs the binfmt handler, or docker starts the
# container and every process in it dies with exec format error.
for a in $arches; do
    [ "$a" = "$(dpkg --print-architecture 2>/dev/null || echo amd64)" ] && continue
    if ! ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | grep -qi "qemu-aarch64\|qemu-arm"; then
        fail "no qemu binfmt handler registered, so a $a container cannot run here.
Install it with:
  docker run --privileged --rm tonistiigi/binfmt --install all"
    fi
    break
done

for arch in $arches; do
    say "Building $arch in $image"
    printf %s "$container_script" | docker run --rm -i \
        --platform "linux/$arch" \
        -v "$repo:/src" \
        "$image" \
        bash -s -- "$arch" "$jobs" "$check" "$(id -u)" "$(id -g)" \
        2>&1 | while IFS= read -r line; do
            case "$line" in
                CHECK_OK*) echo "  decoded ${line#CHECK_OK }" ;;
                *)         echo "  $line" ;;
            esac
        done

    binary=$(ls "$repo"/build-"$arch"/ubersdr-clock_* 2>/dev/null | head -1) \
        || fail "$arch: the build produced no binary"
    cp "$binary" "$repo/"
    built="$built $repo/$(basename "$binary")"
done

say "Done"
for b in $built; do
    printf '  %s\n' "$(file -b "$b" | cut -d, -f1-2) — $(basename "$b")"
done
echo
echo "Copy to the receiver:"
echo "  sudo install -d /opt/ubersdr-clock"
echo "  sudo install -m755 ubersdr-clock_<arch> /opt/ubersdr-clock/"

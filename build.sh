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
# time it was given, then the bundled DCF77, MSF and Allouis recordings
# (tools/testdata/*.wav, I/Q) and checks each reads the minute the
# recording shows.
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
#   --publish       upload what this run built to the 'latest' release, which
#                   is what the UberSDR container downloads at build time
#   --yes           answer the publish confirmation in advance
#

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
arches="amd64 arm64"
image=ubuntu:24.04
native=0
clean=0
check=1
publish=0
assume_yes=0
jobs=$(nproc 2>/dev/null || echo 4)

# The UberSDR Dockerfile fetches
#   https://github.com/$REPO/releases/download/$TAG/ubersdr-clock_${TARGETARCH}
# so the tag is a moving one and the asset names are constants — publishing
# replaces what that build downloads rather than adding alongside it.
REPO="${UBERSDR_CLOCK_REPO:-madpsy/ubersdr-clock}"
TAG="${UBERSDR_CLOCK_TAG:-latest}"

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)     arches=$(echo "$2" | tr ',' ' '); shift 2 ;;
        --native)   native=1; shift ;;
        --clean)    clean=1; shift ;;
        --no-check) check=0; shift ;;
        --image)    image=$2; shift 2 ;;
        --publish)  publish=1; shift ;;
        --yes)      assume_yes=1; shift ;;
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

# --- Two refusals, decided before anything is built ----------------------
#
# Both are about what a download button is allowed to serve, so they fail here
# rather than after a long build.

if [ "$publish" = 1 ] && [ "$check" = 0 ]; then
    fail "--publish and --no-check together would upload a decoder nothing has
watched decode anything. The check is the only thing standing between a clean
compile and a binary that locks onto nothing; a receiver would show it as a
clock that never leaves 'acquiring', which looks like bad propagation rather
than a bad build. Drop one of the two."
fi

if [ "$publish" = 1 ] && [ "$native" = 1 ]; then
    fail "--publish and --native together would upload a binary built against
this host's libstdc++, and it runs inside ubuntu:24.04. If this host is newer
it dies at startup on a GLIBCXX_ version error naming everything except the
real problem. Build it in the container: drop --native."
fi

# --- Publishing ----------------------------------------------------------

publish_release() { # <binary>...
    local uploads=("$@")

    command -v gh >/dev/null 2>&1 || {
        echo "not published: gh not found — install the GitHub CLI, or upload the
  binaries by hand." >&2
        return
    }
    gh auth status >/dev/null 2>&1 || {
        echo "not published: gh is not logged in — run 'gh auth login'." >&2
        return
    }
    gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1 || {
        echo "not published: there is no '$TAG' release on $REPO to upload to." >&2
        return
    }

    echo
    echo "  Upload to https://github.com/$REPO/releases/tag/$TAG, replacing what is there:"
    for b in "${uploads[@]}"; do
        printf '      %-24s %s\n' "$(basename "$b")" "$(du -h "$b" | cut -f1)"
    done
    # Only the arches this run built are replaced. Any other arch already on the
    # release stays exactly as it was, at whatever age it was — so a run with
    # --arch amd64 leaves an arm64 asset from months ago in place, and the
    # release will not say so.
    echo

    # Asked for, one way or the other. The prompt is the default and stays that
    # way: publishing replaces what the container build downloads, and a run
    # that reaches this point by accident must not be able to complete it.
    # `--yes` changes only *when* the answer was given — on the command line
    # rather than at the prompt, which is the same person saying the same thing
    # and is what makes an unattended release possible.
    #
    # A flag rather than an environment variable on purpose: an exported
    # variable is inherited by everything a shell starts, so a `yes` meant for
    # one release would sit there quietly authorising the next.
    if [ "$assume_yes" = 1 ]; then
        echo "  --yes given; uploading."
    elif [ ! -t 0 ]; then
        echo "not published: --publish asks before uploading and there is no terminal
  to ask on. Pass --yes to answer it in advance." >&2
        return
    else
        local reply=''
        read -r -p "  type 'yes' to upload: " reply || true
        if [ "$reply" != "yes" ]; then
            echo "  not published."
            return
        fi
    fi

    # --clobber because the asset names are constants: without it the second
    # release is refused for every name that already exists.
    if gh release upload "$TAG" "${uploads[@]}" --clobber --repo "$REPO"; then
        echo "  uploaded to https://github.com/$REPO/releases/tag/$TAG"
    else
        echo "not published: the upload failed — the binaries are intact, try again." >&2
    fi
}

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
    echo "CHECK_OK WWV $utc quality=$quality"

    # DCF77: a different decoder on a different input (interleaved I/Q), so
    # the WWV check above says nothing about it. Five minutes of real signal,
    # recorded 2026-05-24 around 09:20 UTC. The 44-byte WAV header is dropped,
    # and the plausibility gate is disarmed because the recording is not now.
    wav=/src/tools/testdata/dcf77_live.wav
    if [ ! -f "$wav" ]; then
        echo "no DCF77 recording at $wav to check against" >&2
        exit 1
    fi
    tail -c +45 "$wav" \
        | "$binary" --station dcf77 --no-seconds --diag-seconds 0 --plausibility-minutes 0 \
        > /tmp/dcf77.jsonl
    if ! grep -q '"state":"locked"' /tmp/dcf77.jsonl; then
        echo "built, but never locked on the DCF77 recording" >&2
        tail -3 /tmp/dcf77.jsonl >&2
        exit 1
    fi
    if ! grep -q '"type":"frame","minute":20,"hour":9,"doy":144,"year2":26' /tmp/dcf77.jsonl; then
        echo "DCF77: locked, but did not read 2026-05-24 09:20 UTC from the recording" >&2
        grep '"type":"frame"' /tmp/dcf77.jsonl >&2 || true
        exit 1
    fi
    got=$(grep '"type":"time"' /tmp/dcf77.jsonl | tail -1)
    quality=$(printf %s "$got" | sed -n 's/.*"quality":\([0-9]*\).*/\1/p')
    utc=$(printf %s "$got" | sed -n 's/.*"utc":"\([^"]*\)".*/\1/p')
    echo "CHECK_OK DCF77 $utc quality=$quality"

    # MSF and Allouis: two more decoders on I/Q, each checked on five minutes
    # recorded from M9PSY-1 at 01:20 UTC on 2026-09-25 (doy 268). Both must lock
    # and read the 01:21 UTC frame, the first whole minute in the recording.
    for st in msf:msf allouis:als162; do
        name=${st%%:*}; file=${st##*:}
        wav=/src/tools/testdata/${file}_m9psy1_20260925T0120Z.wav
        if [ ! -f "$wav" ]; then
            echo "no $name recording at $wav to check against" >&2
            exit 1
        fi
        tail -c +45 "$wav" \
            | "$binary" --station "$name" --no-seconds --diag-seconds 0 --plausibility-minutes 0 \
            > /tmp/$name.jsonl
        if ! grep -q '"state":"locked"' /tmp/$name.jsonl; then
            echo "built, but never locked on the $name recording" >&2
            tail -3 /tmp/$name.jsonl >&2
            exit 1
        fi
        if ! grep -q '"type":"frame","minute":21,"hour":1,"doy":268,"year2":26' /tmp/$name.jsonl; then
            echo "$name: locked, but did not read 2026-09-25 01:21 UTC from the recording" >&2
            grep '"type":"frame"' /tmp/$name.jsonl >&2 || true
            exit 1
        fi
        got=$(grep '"type":"time"' /tmp/$name.jsonl | tail -1)
        quality=$(printf %s "$got" | sed -n 's/.*"quality":\([0-9]*\).*/\1/p')
        utc=$(printf %s "$got" | sed -n 's/.*"utc":"\([^"]*\)".*/\1/p')
        echo "CHECK_OK $name $utc quality=$quality"
    done
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
    echo
    echo "Built with this host's toolchain — fine for testing here, not for the"
    echo "container. Drop --native for anything you intend to publish."
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

if [ "$publish" = 1 ]; then
    say "Publishing"
    # shellcheck disable=SC2086  # $built is a deliberate whitespace-separated list
    publish_release $built
else
    echo
    echo "Copy to the receiver:"
    echo "  sudo install -d /opt/ubersdr-clock"
    echo "  sudo install -m755 ubersdr-clock_<arch> /opt/ubersdr-clock/"
    echo
    echo "Or upload both to the '$TAG' release, which is what the UberSDR"
    echo "container build downloads:"
    echo "  ./build.sh --publish"
fi

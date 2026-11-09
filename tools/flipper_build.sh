#!/bin/sh
# Builds SFTV TX with the next build number. flipper/BUILD holds the number of the last build; it
# goes up by one on every build (installed or not) and is independent of the version, which is
# fap_version in flipper/application.fam. Both go into flipper/build_info.h (shown in About).
# Usage: tools/flipper_build.sh          build only
#        tools/flipper_build.sh launch   build, install and start on the Flipper
set -e
cd "$(dirname "$0")/../flipper"
n=$(( $(cat BUILD) + 1 ))
echo "$n" > BUILD
ver=$(sed -n 's/.*fap_version="\([^"]*\)".*/\1/p' application.fam)
printf '#pragma once\n/* written by tools/flipper_build.sh */\n#define SFTV_VERSION "%s"\n#define SFTV_BUILD %d\n' "$ver" "$n" > build_info.h
echo "SFTV TX $ver, build $n"
exec ufbt "$@"

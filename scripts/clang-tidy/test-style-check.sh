#!/usr/bin/env bash
# Native-check regression cases adapted from LabServer's initialization fixtures.
set -euo pipefail
style_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ ${PROTOMATE_STYLE_CONTAINER:-} != 1 ]]; then
    bash "$style_root/scripts/clang-tidy/build-style-check.sh"
    exec docker run --rm --pull=never --network=none --read-only --cap-drop=ALL \
        --security-opt=no-new-privileges --tmpfs /tmp --user "$(id -u):$(id -g)" \
        --env PROTOMATE_STYLE_CONTAINER=1 \
        --mount "type=bind,src=$style_root,dst=$style_root,readonly" \
        --workdir "$style_root" \
        ghcr.io/daleondev/pneumo-devcontainer@sha256:1c10d580f47f4fa402da038896027629ded2d94de4ddd0f382f8a63830fae3e7 \
        bash "$style_root/scripts/clang-tidy/test-style-check.sh"
fi
style_tidy=/opt/clang-p2996/bin/clang-tidy
style_plugin="$style_root/build/clang-tidy-plugin/protomate-style.so"
style_tests="$style_root/scripts/clang-tidy/tests"
style_temporary=$(mktemp -d)
trap 'rm -rf -- "$style_temporary"' EXIT
style_config='{"Checks":"-*,protomate-brace-initialization","WarningsAsErrors":"*","HeaderFilterRegex":".*"}'
"$style_tidy" "--load=$style_plugin" --verify-config "--config-file=$style_root/.clang-tidy"
for fixture in rejected accepted invalid; do
    status=0
    "$style_tidy" "--load=$style_plugin" "--config=$style_config" \
        "--export-fixes=$style_temporary/$fixture.yaml" "$style_tests/$fixture.cpp" -- \
        -std=c++26 -freflection -fexpansion-statements -fannotation-attributes \
        -Wno-c23-extensions -stdlib=libc++ > "$style_temporary/$fixture.log" 2>&1 || status=$?
    case "$fixture" in
        rejected)
            [[ $status == 1 ]] || { cat "$style_temporary/$fixture.log"; exit 1; }
            # 18 distinct declarations/initializers, including the exact user example.
            [[ $(grep -c 'error:.*\[protomate-brace-initialization' "$style_temporary/$fixture.log") == 18 ]]
            grep -q 'double time = 0;' "$style_temporary/$fixture.log"
            ! grep -q 'clang-diagnostic-error' "$style_temporary/$fixture.log"
            ! grep -q 'ReplacementText:' "$style_temporary/$fixture.yaml"
            ;;
        accepted)
            [[ $status == 0 ]] || { cat "$style_temporary/$fixture.log"; exit 1; }
            ;;
        invalid)
            [[ $status == 1 ]] || { cat "$style_temporary/$fixture.log"; exit 1; }
            grep -q 'clang-diagnostic-error' "$style_temporary/$fixture.log"
            ;;
    esac
done
"$style_tidy" "--load=$style_plugin" "--config=$style_config" \
    "$style_tests/accepted.c" -- -std=c17 > "$style_temporary/c.log" 2>&1
printf '%s\n' 'PASS: native brace-initialization check (18 rejected sites, braced C++, C exemption, suppression, parse failure, no unsafe fixes).'

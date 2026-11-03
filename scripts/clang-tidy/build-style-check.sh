#!/usr/bin/env bash
# Native clang-tidy plugin bootstrap adapted from LabServer's build-style-check.sh.
set -euo pipefail

style_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
style_cache="$style_root/build/clang-tidy-plugin"
style_image=ghcr.io/daleondev/pneumo-devcontainer@sha256:1c10d580f47f4fa402da038896027629ded2d94de4ddd0f382f8a63830fae3e7
mkdir -p "$style_cache"
if [[ ${PROTOMATE_STYLE_CONTAINER:-} != 1 ]]; then
    exec docker run --rm --pull=never --read-only --cap-drop=ALL \
        --security-opt=no-new-privileges --tmpfs /tmp --user "$(id -u):$(id -g)" \
        --env PROTOMATE_STYLE_CONTAINER=1 \
        --mount "type=bind,src=$style_root,dst=$style_root,readonly" \
        --mount "type=bind,src=$style_cache,dst=$style_cache" \
        --workdir "$style_root" "$style_image" \
        bash "$style_root/scripts/clang-tidy/build-style-check.sh"
fi

# Match the AST ABI, including reflection additions; never replace the tools.
style_revision=$(/opt/clang-p2996/bin/clang++ --version | sed -n 's/.*clang-p2996.git \([0-9a-f]\{40\}\)).*/\1/p')
if [[ -z $style_revision ]]; then
    printf '%s\n' 'Cannot identify the pinned Clang/P2996 revision.' >&2
    exit 1
fi
style_source="$style_cache/source"
style_headers="$style_cache/headers"
style_plugin="$style_cache/protomate-style.so"
exec 9>"$style_cache/build.lock"
flock 9
style_stamp=$(printf '%s\n' "$style_revision"; sha256sum \
    "$style_root/scripts/clang-tidy/BraceInitializationCheck.cpp" \
    "$style_root/scripts/clang-tidy/build-style-check.sh" /opt/clang-p2996/bin/clang-tidy)
if [[ -f $style_plugin && -f $style_cache/build.stamp && $(cat "$style_cache/build.stamp") == "$style_stamp" ]]; then
    exit 0
fi
if [[ ! -d $style_source/.git ]]; then
    git init -q "$style_source"
    git -C "$style_source" remote add origin https://github.com/bloomberg/clang-p2996.git
    git -C "$style_source" sparse-checkout set --no-cone '/*' '!/*/' '/cmake/' '/llvm/' \
        '!/llvm/test/' '!/llvm/docs/' '!/llvm/examples/' '!/llvm/benchmarks/' \
        '/clang/' '!/clang/test/' '!/clang/docs/' '/clang-tools-extra/clang-tidy/'
fi
if [[ $(git -C "$style_source" rev-parse HEAD 2>/dev/null || true) != "$style_revision" ]]; then
    git -C "$style_source" fetch --depth 1 --filter=blob:none origin "$style_revision"
    git -C "$style_source" checkout --detach "$style_revision"
fi
cmake -S "$style_source/llvm" -B "$style_headers" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/local/bin/gcc -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ \
    -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD= -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF
cmake --build "$style_headers" --target clang-tablegen-targets --parallel "${PROTOMATE_STYLE_JOBS:-4}"
/opt/clang-p2996/bin/clang++ -std=c++20 -stdlib=libstdc++ -shared -fPIC -fno-rtti -O2 \
    -I"$style_source/clang-tools-extra" -I"$style_source/clang/include" \
    -I"$style_source/llvm/include" -I"$style_headers/include" -I"$style_headers/tools/clang/include" \
    "$style_root/scripts/clang-tidy/BraceInitializationCheck.cpp" -o "$style_plugin.tmp"
mv "$style_plugin.tmp" "$style_plugin"
printf '%s\n' "$style_stamp" > "$style_cache/build.stamp"

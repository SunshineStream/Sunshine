#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
base=37ee7d549391938ab5dc468783fc6dadc23dbc7e
lib=a2afc0430d44223c25a2ebc598000f78b338aa52
tag=v2026.1118.30014
src="$root/source"
build="$root/cmake-build-5942"
out="$root/artifacts"
patch="$root/persistence.patch"
runenv=(env "BRANCH=$tag" 'BUILD_VERSION=2026.1118.30014' "COMMIT=$base")

if [[ ${MSYSTEM:-} != UCRT64 ]]; then
  echo 'Run using native Windows MSYS2 UCRT64, not a Linux shell.' >&2
  exit 1
fi
for executable in git cmake ninja cpack node npm pacman unzip sha256sum uv; do
  command -v "$executable" >/dev/null || { echo "Missing required executable: $executable" >&2; exit 1; }
done
for dir in "$src" "$build" "$out"; do
  [[ ! -e "$dir" ]] || { echo "Use a fresh kit directory; already exists: $dir" >&2; exit 1; }
done
git clone --branch "$tag" --depth=1 --recurse-submodules https://github.com/SunshineStream/Sunshine.git "$src"
[[ "$(git -C "$src" rev-parse HEAD)" == "$base" ]]
[[ "$(git -C "$src/third-party/libdisplaydevice" rev-parse HEAD)" == "$lib" ]]
git -C "$src" submodule status --recursive | awk 'substr($0,1,1) != " " {failed=1} END {exit failed}'
git -C "$src" diff --exit-code
git -C "$src" apply --check "$patch"
mkdir -p "$out"
cp "$root/README.md" "$out/TESTING.md"
{
  echo "Sunshine $base ($tag)"
  echo "libdisplaydevice $lib"
  git -C "$src" submodule status --recursive
  cmake --version
  ninja --version
  node --version
  npm --version
  pacman -Q
} > "$out/build-inputs.txt"

configure() {
  "${runenv[@]}" cmake -B "$build" -G Ninja -S "$src" \
    -DBUILD_WERROR=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DDOTNET_EXECUTABLE=OFF \
    -DSUNSHINE_ASSETS_DIR=assets -DSUNSHINE_PUBLISHER_NAME=LizardByte \
    -DSUNSHINE_PUBLISHER_WEBSITE=https://app.lizardbyte.dev \
    -DSUNSHINE_PUBLISHER_ISSUE_URL=https://app.lizardbyte.dev/support
}

build_package() {
  local variant=$1
  "${runenv[@]}" cmake --build "$build"
  rm -f -- "$build/cpack_artifacts/Sunshine.zip"
  (cd "$build" && "${runenv[@]}" cpack -G ZIP --config CPackConfig.cmake)
  local archive="$out/Sunshine-5942-$variant.zip"
  cp "$build/cpack_artifacts/Sunshine.zip" "$archive"
  unzip -tq "$archive"
  local exe
  exe=$(unzip -Z -1 "$archive" | awk '/(^|\/)sunshine[.]exe$/ {print}')
  [[ -n $exe && $exe != *$'\n'* ]] || { echo 'Expected exactly one sunshine.exe in ZIP.' >&2; return 1; }
  unzip -p "$archive" "$exe" | sha256sum | awk '{print $1}' > "$out/$variant-sunshine-exe.sha256"
  if command -v ldd >/dev/null; then ldd "$build/sunshine.exe" > "$out/$variant-linkage.txt"; fi
}

(cd "$src" && uv sync --locked --no-python-downloads --no-install-project) 2>&1 | tee "$out/uv-sync.log"
configure 2>&1 | tee "$out/configure.log"
build_package A-no-save 2>&1 | tee "$out/A-no-save.log"
git -C "$src" apply "$patch"
trap 'git -C "$src" apply --reverse "$patch"' EXIT
git -C "$src" diff --check
[[ "$(git -C "$src" diff --name-only)" == src/display_device.cpp ]]
[[ "$(git -C "$src" diff --numstat)" == $'1\t1\tsrc/display_device.cpp' ]]
git -C "$src" diff -- src/display_device.cpp > "$out/variant-B-source.diff"
build_package B-save 2>&1 | tee "$out/B-save.log"
[[ "$(cat "$out/A-no-save-sunshine-exe.sha256")" != "$(cat "$out/B-save-sunshine-exe.sha256")" ]]
(
  cd "$out"
  sha256sum Sunshine-5942-A-no-save.zip Sunshine-5942-B-save.zip > SHA256SUMS
)
git -C "$src" apply --reverse "$patch"
trap - EXIT
git -C "$src" diff --exit-code
echo "Built two unsigned diagnostic ZIPs in $out. No runtime validation was performed."

#!/usr/bin/env bash
set -euo pipefail

mkdir -p release build
for tool in linuxdeploy linuxdeploy-plugin-qt; do
  curl --fail --location --retry 3 "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-x86_64.AppImage" -o "build/$tool-x86_64.AppImage"
  chmod +x "build/$tool-x86_64.AppImage"
done

resource=build/app/TrenchBroom
cp -R docs "$resource/docs"
cp README*.md ULTIMATE*.md FRACTURE*.md PERFORATE*.md SETTLE*.md THIRD_PARTY_NOTICES*.md "$resource/"
mkdir -p "$resource/licenses/Qt" "$resource/licenses/vcpkg"
while IFS= read -r notice; do
  package=$(basename "$(dirname "$notice")")
  mkdir -p "$resource/licenses/vcpkg/$package"
  cp "$notice" "$resource/licenses/vcpkg/$package/copyright"
done < <(find build/vcpkg_installed/x64-linux/share -name copyright -type f)
qt_version=$("$QT_ROOT_DIR/bin/qmake" -query QT_VERSION)
for license in LGPL-3.0-only.txt GPL-3.0-only.txt; do
  curl --fail --location --retry 3 "https://raw.githubusercontent.com/qt/qtbase/v$qt_version/LICENSES/$license" -o "$resource/licenses/Qt/$license"
done
python3 - "$qt_version" <<'PY'
import json, os, pathlib, sys
info = {'commit': os.environ['GITHUB_SHA'], 'qt': sys.argv[1],
        'platform': 'Linux x86_64; built on Ubuntu 24.04',
        'source': 'https://github.com/' + os.environ['GITHUB_REPOSITORY'] + '/tree/' + os.environ['GITHUB_SHA']}
pathlib.Path('release/linux-build-info.json').write_text(json.dumps(info, indent=2))
PY
cp release/linux-build-info.json "$resource/build-info.json"

# Add release documentation to the staging installation used by CPack.
cat >> build/app/TrenchBroom/cmake_install.cmake <<EOF
file(INSTALL DESTINATION "\${CMAKE_INSTALL_PREFIX}/share/TrenchBroom" TYPE DIRECTORY FILES "$PWD/$resource/docs" "$PWD/$resource/licenses")
file(INSTALL DESTINATION "\${CMAKE_INSTALL_PREFIX}/share/TrenchBroom" TYPE FILE FILES "$PWD/$resource/build-info.json" "$PWD/README.md" "$PWD/README.ru.md" "$PWD/ULTIMATE.md" "$PWD/ULTIMATE.ru.md" "$PWD/THIRD_PARTY_NOTICES.md" "$PWD/THIRD_PARTY_NOTICES.ru.md")
EOF
(cd build && cpack)
mv build/TrenchBroom.AppImage release/ReyondBroom-Linux-x86_64.AppImage
chmod +x release/ReyondBroom-Linux-x86_64.AppImage
mkdir -p build/appimage-test
(cd build/appimage-test && ../../release/ReyondBroom-Linux-x86_64.AppImage --appimage-extract >/dev/null)
test -f build/appimage-test/squashfs-root/usr/share/TrenchBroom/build-info.json
test -f build/appimage-test/squashfs-root/usr/share/TrenchBroom/licenses/Qt/LGPL-3.0-only.txt
env -u QT_PLUGIN_PATH -u QML2_IMPORT_PATH -u LD_LIBRARY_PATH xvfb-run -a env QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 timeout 90 build/appimage-test/squashfs-root/AppRun --capture-guide "$PWD/scripts/release/linux-smoke.json"
python3 - <<'PY'
import json, pathlib
result = json.loads(pathlib.Path('scripts/release/linux-smoke/capture-result.json').read_text())
assert result['success'], result
print('Packaged AppImage OpenGL smoke test passed:', result)
PY
(cd release && sha256sum ReyondBroom-Linux-x86_64.AppImage > ReyondBroom-Linux-x86_64.AppImage.sha256)

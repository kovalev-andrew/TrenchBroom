# Third-Party Notices

**English (primary)** · [Russian](THIRD_PARTY_NOTICES.ru.md)

ReyondBroom retains TrenchBroom's and UltimateTrenchBroom's upstream credits and copyright notices. The editor is licensed under GPL-3.0-or-later; see LICENSE.txt.

The Windows archive includes license notices for installed vcpkg dependencies under `licenses/vcpkg/<package>/copyright`, the Source Sans Pro font license under `fonts`, and the Qt LGPL license under `licenses/Qt`.

Qt 6.10.x libraries are dynamically linked. The exact version is recorded in `build-info.json`; its upstream source is available in the matching version directory of [Qt's 6.10 source archive](https://download.qt.io/archive/qt/6.10/). The DLLs remain separate and replaceable.

Other dependencies include Assimp, FreeImage, FreeType, TinyXML2, miniz, cpptrace, fmt, and their image/compression dependencies. Their individual notices are bundled rather than replaced by the editor's license.

Microsoft Visual C++ runtime components, when included, are redistributed from the Visual Studio x64 release CRT. They retain Microsoft's license terms.

Source code and build instructions are available on the [ReyondBroom branch](https://github.com/reyond9079/ReyondBroom/tree/ReyondBroom). `build-info.json` links to the exact source commit used for the archive. Initialize the vcpkg submodule when building from Git. Imported tools originate from [dragxnd/UltimateTrenchBroom](https://github.com/dragxnd/UltimateTrenchBroom).

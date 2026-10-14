# Third-Party Notices

Ultimate Trench Broom retains TrenchBroom's upstream credits and copyright notices. The editor is licensed under GPL-3.0-or-later; see LICENSE.txt.

The Windows archive includes license notices for installed vcpkg dependencies under `licenses/vcpkg/<package>/copyright`, the Source Sans Pro font license under `fonts`, and the Qt LGPL license under `licenses/Qt`.

Qt 6.9.2 libraries are dynamically linked. Their upstream source is available from [Qt's 6.9.2 source archive](https://download.qt.io/archive/qt/6.9/6.9.2/single/). The DLLs remain separate and replaceable.

Other dependencies include Assimp, FreeImage, FreeType, TinyXML2, miniz, cpptrace, fmt, and their image/compression dependencies. Their individual notices are bundled rather than replaced by the editor's license.

Microsoft Visual C++ runtime components, when included, are redistributed from the Visual Studio x64 release CRT. They retain Microsoft's license terms.

Source code and build instructions for this release are available at [the v0.4 tag](https://github.com/dragxnd/UltimateTrenchBroom/tree/v0.4). Initialize the vcpkg submodule when building from Git.

# Сторонние компоненты

[English (основной)](THIRD_PARTY_NOTICES.md) · **Русский**

ReyondBroom сохраняет сведения об авторах и авторских правах TrenchBroom и
UltimateTrenchBroom. Редактор распространяется под GPL-3.0-or-later; см. LICENSE.txt.

Архив Windows содержит лицензии зависимостей vcpkg в
`licenses/vcpkg/<пакет>/copyright`, лицензию шрифта Source Sans Pro в `fonts`
и лицензии Qt LGPL/GPL в `licenses/Qt`.

Библиотеки Qt 6.10.x подключены динамически: DLL остаются отдельными и заменяемыми.
Точная версия записана в `build-info.json`. Исходники соответствующей версии
находятся в [архиве Qt 6.10](https://download.qt.io/archive/qt/6.10/).

Используются также Assimp, Bullet, FreeImage, FreeType, TinyXML2, miniz, cpptrace,
fmt и их зависимости. Собственные лицензии этих компонентов включаются в архив.
Компоненты среды выполнения Visual C++, если они включены, сохраняют условия Microsoft.

Исходники и инструкции сборки находятся в
[ветке ReyondBroom](https://github.com/reyond9079/TrenchBroom/tree/ReyondBroom).
`build-info.json` содержит ссылку на точный коммит сборки. При получении исходников
инициализируйте подмодуль vcpkg. Перенесённые инструменты созданы в
[dragxnd/UltimateTrenchBroom](https://github.com/dragxnd/UltimateTrenchBroom).

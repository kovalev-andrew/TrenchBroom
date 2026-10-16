# ReyondBroom

[![Скачать для Windows x64](https://img.shields.io/badge/Скачать_для_Windows_x64-0078D4?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Windows.zip)
[![Скачать для Linux x86_64](https://img.shields.io/badge/Скачать_для_Linux_x86__64-FCC624?style=for-the-badge&logo=linux&logoColor=black)](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Linux-x86_64.AppImage)

[Все загрузки, контрольные суммы SHA-256 и описание релиза](https://github.com/reyond9079/ReyondBroom/releases/latest)

[English (основной)](README.md) · **Русский**

ReyondBroom — набор инструментов для создания и размещения обломков из брашей
на основе TrenchBroom v2026.2. Разработка ведётся в
[ветке ReyondBroom](https://github.com/reyond9079/ReyondBroom/tree/ReyondBroom).

- **[Perforate](PERFORATE.ru.md)** создаёт случайные сквозные отверстия с заданными
  количеством, числом сторон, диапазоном размеров и промежутками. Можно оставить
  и выделить браши-пробки как отдельные `func_wall`.
- **[Bevel](BEVEL.ru.md)** скругляет внешний контур выбранных поверхностей, рёбра
  или вершины: предпросмотр на сцене, радиус, сегменты, выпуклый профиль и Clamp отдельно для каждого браша.
- **[Fracture](FRACTURE.ru.md)** разделяет один браш на 2–128 выпуклых осколков
  Вороного. Есть seed, материал срезов, каркасный предпросмотр и отмена одним Undo.
- **[Settle](SETTLE.ru.md)** рассчитывает падение и столкновения 1–128 выделенных
  брашей. Есть трение, отскок, начальный толчок, Pause/Resume и отменяемое применение.

## Быстрый запуск

Скачайте [ReyondBroom для Windows x64](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Windows.zip).
Распакуйте ZIP в отдельную папку и запустите `TrenchBroom.exe`.
Исполняемый файл сохраняет название исходного редактора.

Для **Linux x86_64** скачайте [AppImage](https://github.com/reyond9079/ReyondBroom/releases/latest/download/ReyondBroom-Linux-x86_64.AppImage).
Он собран и проверен на Ubuntu 24.04, Qt включён; старые дистрибутивы не проверены.
Разрешите запуск файла и откройте редактор:

```sh
chmod +x ReyondBroom-Linux-x86_64.AppImage
./ReyondBroom-Linux-x86_64.AppImage
```

Если FUSE недоступен, добавьте при запуске `--appimage-extract-and-run`.

Выделите браш → **Edit → CSG → Fracture…** → **Apply**. Сохраняя выделение
осколков, откройте **Edit → CSG → Settle…** → **Simulate / Restart** → **Apply**.
Заранее создайте невыделенный браш пола: Settle не добавляет автоматическую поверхность.

Основная документация ведётся на английском, русские переводы обновляются вместе
с ней. Все комментарии в исходном коде должны быть на английском.
Правила разработки находятся в [AGENTS.md](AGENTS.md), требования к сборке —
в [BUILD.md](BUILD.md). Инструменты экспериментальные: проверяйте результат
BSP-компилятором вашей игры. Лицензия GPL и авторство исходного проекта сохранены.

## Дополнительные инструменты

Перенесён набор поддерживаемых возможностей
[UltimateTrenchBroom](https://github.com/dragxnd/UltimateTrenchBroom) автора dragxnd.
Список инструментов и особенности совместимости приведены в
[руководстве](ULTIMATE.ru.md) ([English](ULTIMATE.md)).
Авторство исходных проектов и лицензия GPL сохранены.

## Исходный проект

TrenchBroom — редактор уровней для игр на движках семейства Quake.
Сведения об исходном редакторе сохранены в [английском README](README.md#upstream-trenchbroom).

- [Репозиторий TrenchBroom](https://github.com/TrenchBroom/TrenchBroom)
- [Руководство TrenchBroom](https://trenchbroom.github.io/manual/latest)

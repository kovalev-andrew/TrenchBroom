# ReyondBroom

[English (основной)](README.md) · **Русский**

ReyondBroom — набор инструментов для создания и размещения обломков из брашей
на основе TrenchBroom v2026.2. Разработка ведётся в
[ветке ReyondBroom](https://github.com/reyond9079/ReyondBroom/tree/ReyondBroom).

- **[Perforate](PERFORATE.ru.md)** создаёт случайные сквозные отверстия с заданными
  количеством, числом сторон, диапазоном размеров и промежутками. Можно оставить
  и выделить браши-пробки как отдельные `func_wall`.
- **[Fracture](FRACTURE.ru.md)** разделяет один браш на 2–128 выпуклых осколков
  Вороного. Есть seed, материал срезов, каркасный предпросмотр и отмена одним Undo.
- **[Settle](SETTLE.ru.md)** рассчитывает падение и столкновения 1–128 выделенных
  брашей. Есть трение, отскок, начальный толчок, Pause/Resume и отменяемое применение.

## Быстрый запуск

Скачайте артефакт `ReyondBroom-Windows` из успешной
[Windows-сборки](https://github.com/reyond9079/ReyondBroom/actions/workflows/fracture.yml?query=branch%3AReyondBroom).
Распакуйте артефакт и вложенный `ReyondBroom-Windows.zip` в отдельную папку,
затем запустите `TrenchBroom.exe`. Исполняемый файл сохраняет название исходного редактора.

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

# reyondboom

[English (основной)](README.md) · **Русский**

reyondboom — набор инструментов для создания и размещения обломков из брашей
на основе TrenchBroom v2026.2. Разработка ведётся в
[ветке reyondboom](https://github.com/reyond9079/TrenchBroom/tree/reyondboom).

- **[Fracture](FRACTURE.ru.md)** разделяет один браш на 2–128 выпуклых осколков
  Вороного. Есть seed, материал срезов, каркасный предпросмотр и отмена одним Undo.
- **[Settle](SETTLE.ru.md)** рассчитывает падение и столкновения 1–128 выделенных
  брашей. Есть трение, отскок, начальный толчок, Pause/Resume и отменяемое применение.

## Быстрый запуск

Скачайте артефакт `reyondboom-Windows` из успешной
[Windows-сборки](https://github.com/reyond9079/TrenchBroom/actions/workflows/fracture.yml?query=branch%3Areyondboom).
Распакуйте артефакт и вложенный `reyondboom-Windows.zip` в отдельную папку,
затем запустите `TrenchBroom.exe`. Исполняемый файл сохраняет название исходного редактора.

Выделите браш → **Edit → CSG → Fracture…** → **Apply**. Сохраняя выделение
осколков, откройте **Edit → CSG → Settle…** → **Simulate / Restart** → **Apply**.
Заранее создайте невыделенный браш пола: Settle не добавляет автоматическую поверхность.

Основная документация ведётся на английском, русские переводы обновляются вместе
с ней. Все комментарии в исходном коде должны быть на английском.
Правила разработки находятся в [AGENTS.md](AGENTS.md), требования к сборке —
в [BUILD.md](BUILD.md). Инструменты экспериментальные: проверяйте результат
BSP-компилятором вашей игры. Лицензия GPL и авторство исходного проекта сохранены.

## Исходный проект

TrenchBroom — редактор уровней для игр на движках семейства Quake.
Сведения об исходном редакторе сохранены в [английском README](README.md#upstream-trenchbroom).

- [Репозиторий TrenchBroom](https://github.com/TrenchBroom/TrenchBroom)
- [Руководство TrenchBroom](https://trenchbroom.github.io/manual/latest)

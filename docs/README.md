# ReyondBroom documentation

English · [Русский](README.ru.md)

[English handbook](https://reyond9079.github.io/ReyondBroom/) · [Russian handbook](https://reyond9079.github.io/ReyondBroom/index.ru.html) · [Full reference](https://reyond9079.github.io/ReyondBroom/reference.html)

The handbook source is `src/handbook.json`: every chapter has matching English and Russian text, a selection/entry route, ordered steps, constraints and screenshot paths. English is authoritative. `site.css` and `site.js` provide responsive layout, keyboard-accessible navigation and local text search; no analytics or external fonts are used.

Install Python 3, Pandoc and the packages in `requirements.txt`, then run from the repository root:

```sh
python -m pip install -r docs/requirements.txt
python scripts/build-handbook.py --output _site
python -m http.server 8765 --directory _site
```

Open `http://localhost:8765`. The build validates local links, anchors, image files and alt text. `_site` is generated output; do not edit it. The small `docs/index.html` is a repository entry point; the complete site is built by the Pages workflow.

The complete reference is generated from the maintained core manual source under `app/TrenchBroom/resources/documentation/manual`. The build adapts prose and support/update instructions while preserving code examples, internal filenames, original source metadata and notices. `src/reference-shortcuts.js` is the shortcut table from release `f12556d`; refresh it from a newly built editor when menu paths or defaults change.

Before publishing, inspect the English and Russian pages, search, navigation, reference macros, desktop and narrow layouts, and several full-resolution figures. Keep the image inventory and revision notes in `src/sources.html` accurate. Do not invent UI screenshots or remove original notices from reused sources.

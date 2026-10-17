#!/usr/bin/env python3
"""Build the bilingual handbook and complete core reference without compiling the editor."""
import argparse
import html
import json
import re
import shutil
import subprocess
from pathlib import Path

from bs4 import BeautifulSoup

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / 'docs'
MANUAL = ROOT / 'app/TrenchBroom/resources/documentation/manual'
RELEASES = 'https://github.com/reyond9079/ReyondBroom/releases/latest'
SOURCE = 'https://github.com/reyond9079/ReyondBroom'
GROUPS_RU = {'Start here':'Начало работы', 'Core editing':'Основы редактирования',
             'Shape and generate':'Форма и генераторы', 'Repeat and align':'Повторение и точность',
             'Assets and export':'Ресурсы и экспорт', 'Game previews':'Игровые предпросмотры',
             'Bevel and destruction':'Скругление и разрушение', 'Troubleshooting':'Практика и диагностика'}

def esc(value):
    return html.escape(str(value), quote=True)

def shell(lang, title, nav, body, extra_head=''):
    ru = lang == 'ru'
    return f'''<!doctype html>
<html lang="{lang}"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{esc(title)} — ReyondBroom</title><meta name="description" content="ReyondBroom level editor: illustrated workflows, tools and reference.">
<link rel="stylesheet" href="site.css">{extra_head}<script src="site.js" defer></script></head>
<body><a class="skip" href="#main">{'К содержимому' if ru else 'Skip to content'}</a>
<header><a class="brand" href="index{'.ru' if ru else ''}.html">Reyond<span>Broom</span></a>
<div class="header-links"><a href="{RELEASES}">{'Скачать' if ru else 'Download'}</a><a href="{SOURCE}">GitHub</a>
<a class="language" href="index.html" lang="en">English</a><a class="language" href="index.ru.html" lang="ru">Русский</a></div></header>
<div class="layout"><nav aria-label="{'Содержание' if ru else 'Contents'}">{nav}</nav>
<main id="main">{body}<footer><a href="sources.html">{'Источники и лицензии' if ru else 'Sources and licenses'}</a> ·
<a href="reference.html">{'Полный справочник (English)' if ru else 'Full reference'}</a> · 2026.2-reyond.2</footer></main></div></body></html>'''

def make_handbook(sections, lang):
    ru = lang == 'ru'
    title = 'Руководство пользователя' if ru else 'User handbook'
    nav = f'<label for="search">{"Найти инструмент или действие" if ru else "Find a tool or task"}</label><input id="search" type="search" placeholder="{"Поиск по руководству" if ru else "Search the handbook"}"><p id="search-status" role="status"></p>'
    nav += '<a href="reference.html">' + ('Полный справочник (EN)' if ru else 'Full core reference') + ' ↗</a>'
    body = f'''<div class="hero"><p class="eyebrow">LEVEL EDITOR / USER GUIDE</p><h1>{title}</h1>
<p class="lead">{'От первого браша до готового уровня: геометрия, материалы, сущности, генераторы и размещение обломков.' if ru else 'From the first brush to a finished level: geometry, materials, entities, generators and rubble placement.'}</p>
<div class="buttons"><a class="button" href="#start">{'Начать работу' if ru else 'Start building'}</a><a class="button secondary" href="#bevel">Bevel</a><a class="button secondary" href="#fracture">Fracture & Settle</a></div>
<p class="edition">{'Версия 2026.2-reyond.2 · 45 глав · English / Русский' if ru else 'Version 2026.2-reyond.2 · 45 chapters · English / Русский'}</p></div>
<aside class="notice">{'Названия элементов интерфейса сохранены на английском. Снимки базовых и дополнительных инструментов могут показывать прежнее оформление; текст учитывает текущую версию. Нажмите изображение для полного размера. Источники собраны в одном справочном разделе.' if ru else 'Interface labels are kept in English. Core and extended-tool illustrations may show an earlier appearance; the instructions account for this version. Click any image for full resolution. Image origins are recorded in one reference section.'}</aside>'''
    groups = list(dict.fromkeys(s['group'] for s in sections))
    for group in groups:
        label = GROUPS_RU[group] if ru else group
        nav += f'<div class="nav-group"><h2>{esc(label)}</h2>'
        for section in [s for s in sections if s['group'] == group]:
            d = section[lang]; id = section['id']
            nav += f'<a href="#{id}" data-chapter="{id}">{esc(d["title"])}</a>'
            body += f'<section id="{id}" class="chapter"><p class="eyebrow">{esc(label)}</p><h2>{esc(d["title"])}</h2><p class="intro">{esc(d["intro"])}</p>'
            if section['route']:
                body += f'<p class="route">{esc(section["route"])}</p>'
            body += '<ol class="steps">' + ''.join(f'<li>{esc(step)}</li>' for step in d['steps']) + '</ol>'
            body += f'<aside class="tip"><strong>{"Параметры и ограничения" if ru else "Controls and limits"}</strong><p>{esc(d["notes"])}</p></aside>'
            if section['images']:
                body += '<div class="figures">'
                for i, image in enumerate(section['images']):
                    figure_label = (f'{d["title"]} — иллюстрация {i+1}' if ru else f'{d["title"]} — illustration {i+1}')
                    caption = section.get('captions', {}).get(lang, [])
                    if i < len(caption): figure_label = caption[i]
                    body += f'<figure><a href="{esc(image)}" target="_blank" rel="noopener"><img loading="lazy" decoding="async" src="{esc(image)}" alt="{esc(figure_label)}"></a><figcaption>{esc(figure_label)}</figcaption></figure>'
                body += '</div>'
            body += '<a class="back" href="#main">↑ ' + ('К началу' if ru else 'Back to top') + '</a></section>'
        nav += '</div>'
    return shell(lang, title, nav, body)

def build_reference(pandoc):
    source = (MANUAL/'index.md').read_text(encoding='utf-8')
    source = re.sub(r'^%.*\n', '', source, flags=re.M)
    # Keep game configuration syntax, internal filenames and code examples intact.
    # Only visible prose nodes are rebranded after conversion.
    source = re.sub(r'## Automatic Updates\n.*?(?=## Command Repetition)',
                    '## Updating ReyondBroom\n\nDownload new builds from [ReyondBroom Releases](' + RELEASES + '). Keep maps and project data outside the application folder. Do not use another editor’s updater as a ReyondBroom release channel.\n\n', source, flags=re.S)
    reference_links = source[source.index('[TrenchBroom on GitHub]:'):]
    source = source[:source.index('# Getting Involved')]
    source += '\n# Support and version information {#reporting_bugs}\n\nUse the ReyondBroom repository for this editor. Include the build version, platform, a minimal map and reproduction steps. The packaged build-info.json identifies the exact source revision.\n\n# Reference sources {#references_and_links}\n\nSee [Sources and licenses](sources.html) for the source history of this reference and its illustrations.\n\n' + reference_links
    for macro, function in [('menu', 'print_menu_item'), ('action', 'print_action'), ('key', 'print_key')]:
        source = re.sub(r'#'+macro+r'\(([^)]+)\)', lambda m: '<script>'+function+'('+json.dumps(m[1])+');</script>', source)
    result = subprocess.run([pandoc, '--from=markdown', '--to=html5', '--wrap=none'], input=source, text=True, encoding='utf-8', capture_output=True, check=True).stdout
    soup = BeautifulSoup(result, 'html.parser')
    for text in list(soup.find_all(string=True)):
        if text.parent.name not in ('code', 'pre', 'script') and not text.find_parent('pre') and not text.find_parent('code'):
            text.replace_with(str(text).replace('TrenchBroom', 'ReyondBroom'))
    for image in soup.find_all('img'):
        image['src'] = 'reference/' + image['src']
        image['loading'] = 'lazy'
        if not image.get('alt'):
            image['alt'] = Path(image['src']).stem
    for link in soup.find_all('a', href='#entity_browser'):
        link['href'] = '#creating_entities'
    # Core menu paths are resolved from the released application's shortcut table.
    nav = '<a href="index.html">← User handbook</a><h2>Core reference</h2>'
    for heading in soup.find_all(['h1', 'h2']):
        nav += f'<a href="#{esc(heading.get("id", ""))}">{esc(heading.get_text())}</a>'
    body = '<div class="hero"><p class="eyebrow">REFERENCE / ENGLISH</p><h1>Core editor reference</h1><p class="lead">Detailed editing behaviour, map compilation, expressions and game configuration.</p></div><aside class="notice">Adapted for ReyondBroom from the core manual shipped with this source tree. The <a href="index.html">handbook</a> documents the additional tools and supersedes older tool descriptions. Internal filenames and configuration keys retain their actual names. Original reference authorship is retained in <a href="sources.html">Sources and licenses</a>.</aside>'
    return shell('en', 'Core editor reference', nav, body + '<article class="reference">'+str(soup)+'</article>', '<script src="reference/handbook-shortcuts.js"></script><script src="reference/shortcuts_helper.js"></script>')

def validate(output):
    errors = []
    for file in output.glob('*.html'):
        soup = BeautifulSoup(file.read_text(encoding='utf-8'), 'html.parser')
        ids = [x['id'] for x in soup.find_all(id=True)]
        if len(ids) != len(set(ids)): errors.append(f'{file.name}: duplicate IDs')
        for element in soup.find_all(['a','img','script','link']):
            value = element.get('href') or element.get('src') or ''
            if not value or re.match(r'^(https?:|mailto:|data:)', value): continue
            path, _, anchor = value.partition('#')
            target = (file.parent/path).resolve() if path else file
            if not target.exists(): errors.append(f'{file.name}: missing {value}')
            elif anchor and target.suffix == '.html':
                target_soup = soup if target == file else BeautifulSoup(target.read_text(encoding='utf-8'), 'html.parser')
                if not target_soup.find(id=anchor): errors.append(f'{file.name}: missing anchor {value}')
        for image in soup.find_all('img'):
            if not image.get('alt'): errors.append(f'{file.name}: image without alt')
    if errors: raise SystemExit('\n'.join(errors))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT/'_site')
    parser.add_argument('--pandoc', default='pandoc')
    args = parser.parse_args(); output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    sections = json.loads((DOCS/'src/handbook.json').read_text(encoding='utf-8'))
    for name in ('captures', 'screenshots'):
        shutil.copytree(DOCS/name, output/name, dirs_exist_ok=True)
    for name in ('site.css', 'site.js'):
        shutil.copyfile(DOCS/name, output/name)
    shutil.copytree(MANUAL/'images', output/'reference/images', dirs_exist_ok=True)
    shutil.copyfile(MANUAL/'shortcuts_helper.js', output/'reference/shortcuts_helper.js')
    shutil.copyfile(DOCS/'src/reference-shortcuts.js', output/'reference/handbook-shortcuts.js')
    shutil.copyfile(ROOT/'LICENSE.txt', output/'LICENSE.txt')
    shutil.copyfile(ROOT/'THIRD_PARTY_NOTICES.md', output/'THIRD_PARTY_NOTICES.md')
    for lang in ('en','ru'):
        (output/('index.ru.html' if lang == 'ru' else 'index.html')).write_text(make_handbook(sections, lang), encoding='utf-8')
    (output/'reference.html').write_text(build_reference(args.pandoc), encoding='utf-8')
    (output/'sources.html').write_text(shell('en', 'Sources and licenses', '<a href="index.html">← English handbook</a><a href="index.ru.html">← Руководство</a>', (DOCS/'src/sources.html').read_text(encoding='utf-8')), encoding='utf-8')
    (output/'.nojekyll').write_text('', encoding='utf-8')
    validate(output)
    print(f'Built and validated {len(sections)} bilingual chapters and the complete core reference in {output}')

if __name__ == '__main__': main()

/* Search keeps the page usable offline and does not transmit queries. */
const search = document.querySelector('#search');
if (search) {
  const chapters = [...document.querySelectorAll('.chapter')];
  const status = document.querySelector('#search-status');
  const update = () => {
    const words = search.value.toLocaleLowerCase().trim().split(/\s+/).filter(Boolean);
    let count = 0;
    for (const chapter of chapters) {
      const text = chapter.textContent.toLocaleLowerCase();
      const show = words.every(word => text.includes(word));
      chapter.hidden = !show;
      document.querySelector(`[data-chapter="${chapter.id}"]`).hidden = !show;
      if (show) count++;
    }
    for (const group of document.querySelectorAll('.nav-group')) {
      group.hidden = ![...group.querySelectorAll('a')].some(a => !a.hidden);
    }
    status.textContent = words.length ? (document.documentElement.lang === 'ru' ? `Найдено глав: ${count}` : `${count} chapters found`) : '';
  };
  search.addEventListener('input', update);
  window.addEventListener('hashchange', () => {
    const target = document.getElementById(location.hash.slice(1));
    if (target?.hidden) { search.value = ''; update(); target.scrollIntoView(); }
  });
}
// Keep chapter context when switching handbook languages.
for (const link of document.querySelectorAll('a.language')) {
  link.addEventListener('click', () => {
    if (document.querySelector('.chapter') && location.hash) link.href = link.getAttribute('href').split('#')[0] + location.hash;
  });
}

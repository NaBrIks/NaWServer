// SPDX-License-Identifier: MIT
// Render the sanitized hardware observations, without contacting any device.
import {readFileSync, writeFileSync, mkdirSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const data = JSON.parse(readFileSync(resolve(root, 'examples/compact/README_PROFILE_RESULTS.json'), 'utf8'));
const output = resolve(root, 'docs/images');
mkdirSync(output, {recursive: true});
const escape = value => String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
const number = (value, lang, digits = 1) => value.toFixed(digits).replace('.', lang === 'ru' ? ',' : '.');
const pct = (part, whole) => part * 100 / whole;
const text = (x, y, value, size = 14, color = '#352f28', extra = '') =>
  `<text x="${x}" y="${y}" font-size="${size}" fill="${color}" ${extra}>${escape(value)}</text>`;
const rect = (x, y, width, height, color) => `<rect x="${x}" y="${y}" width="${width}" height="${height}" fill="${color}"/>`;
const begin = (height, title, description) => `<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="${height}" viewBox="0 0 1100 ${height}" role="img" aria-labelledby="title desc"><title id="title">${escape(title)}</title><desc id="desc">${escape(description)}</desc><rect width="1100" height="${height}" fill="#faf8f3"/><g font-family="Segoe UI,Arial,sans-serif">`;
const profile = (row, lang) => (row.board === 'c3' ? 'C3' : 'S3') + ' · ' +
  (lang === 'ru' ? ['Мини', 'Нормальный', 'Максимальный'] : ['Mini', 'Normal', 'Maximum'])[row.profile - 1];

for (const lang of ['ru', 'en']) {
  const ru = lang === 'ru';
  const kib = ru ? 'КиБ' : 'KiB';
  const title = ru ? 'SRAM: занято и свободно в трёх профилях' : 'SRAM: usage and headroom in three profiles';
  let svg = begin(650, title, 'All bars and segments share one linear byte scale starting at zero. Pre-start usage is split into resident IRAM code, SDK/C++ globals, directly attributed library/demo static storage, and remaining pre-HTTP heap plus reserves. These are not all NaWServer costs.');
  svg += text(24, 32, title, 22);
  svg += text(24, 56, ru ? 'C3: 400 КиБ · S3: 512 КиБ · PSRAM выключена · 45 с обмена · единая линейная шкала в КиБ' : 'C3: 400 KiB · S3: 512 KiB · PSRAM off · 45 s traffic · one shared linear scale in KiB', 14);
  const colors = ['#8996a4', '#b4c0c9', '#667f9b', '#d5dce2', '#cb8b31', '#e0c37a', '#73ad91'];
  const legends = ru ? ['Код в SRAM (IRAM)', 'Глобальные данные SDK/C++', 'Наши статические данные', 'До HTTP: heap и резервы', 'HTTP: запуск', 'HTTP/TCP: обмен', 'Свободно'] :
    ['Code in SRAM (IRAM)', 'SDK/C++ global data', 'Our static data', 'Pre-HTTP heap + reserves', 'HTTP startup', 'HTTP/TCP traffic', 'Free'];
  for (const [i, x, y] of [[0,24,78],[1,350,78],[3,740,78],[2,24,104],[4,350,104],[5,580,104],[6,840,104]]) {
    svg += rect(x, y, 14, 14, colors[i]) + text(x + 22, y + 12, legends[i]);
  }
  const startX = 190, width = 880;
  const maximumBytes = Math.max(...data.rows.map(row => row.physical_bytes));
  const pixelsPerByte = width / maximumBytes;
  for (let i = 0; i <= 8; ++i) {
    const x = startX + width * i / 8;
    svg += text(x, 145, number(maximumBytes * i / 8 / 1024, lang, 0), 12, '#71675c', 'text-anchor="middle"');
    svg += `<path d="M${x} 154V574" stroke="#e1dace" stroke-width="1"/>`;
  }
  for (const [index, row] of data.rows.entries()) {
    const y = 166 + index * 68 + (index >= 3 ? 12 : 0);
    const before = row.before_start_breakdown;
    const ownStatic = before.library_archive_resident_bytes + before.demo_archive_resident_bytes;
    const parts = [before.iram_bytes, before.globals_bytes - ownStatic, ownStatic,
      before.system_heap_and_reserves_bytes, row.startup_heap_bytes,
      row.heap_after - row.minimum_sampled_free_bytes, row.minimum_sampled_free_bytes];
    if (parts.some(x => x < 0) || parts.reduce((a, b) => a + b) !== row.physical_bytes) throw Error('Invalid memory partition');
    svg += text(24, y + 20, profile(row, lang), 14);
    let x = startX;
    parts.forEach((part, i) => {
      const size = part * pixelsPerByte;
      svg += rect(x, y, size, 28, colors[i]);
      if (i === 0 || i === 1 || i === 3) svg += text(x + size / 2, y + 19, number(part / 1024, lang), 13, '#303b46', 'text-anchor="middle"');
      x += size;
    });
    svg += text(x - 8, y + 19, `${number(parts[6] / 1024, lang)} ${kib} · ${number(pct(parts[6], row.physical_bytes), lang)}%`, 14, '#183d2b', 'text-anchor="end"');
    const mini = data.rows.find(v => v.board === row.board && v.profile === 1);
    const delta = pct(row.startup_heap_bytes - mini.startup_heap_bytes, mini.startup_heap_bytes);
    svg += text(startX, y + 47, ru ? `Наши static ${number(ownStatic / 1024, lang)} · HTTP старт ${number(parts[4] / 1024, lang)} (+${number(delta, lang)}% к Мини) · обмен ${number(parts[5] / 1024, lang)} КиБ` :
      `Our static ${number(ownStatic / 1024, lang)} · HTTP startup ${number(parts[4] / 1024, lang)} (+${number(delta, lang)}% vs Mini) · traffic ${number(parts[5] / 1024, lang)} KiB`, 12, '#655c50');
    svg += text(x, y + 47, `${ru ? 'Всего' : 'Total'} ${number(row.physical_bytes / 1024, lang, 0)} ${kib}`, 12, '#655c50', 'text-anchor="end"');
  }
  svg += text(24, 610, ru ? 'Наши static: прямые секции NaWServer + демо. Общие зависимости C++/SDK посчитаны отдельно.' : 'Our static: direct NaWServer + demo sections. Shared C++/SDK dependencies are shown separately.', 13);
  svg += text(24, 632, ru ? 'До HTTP: heap и резервы ОС/сети/приложения; точной атрибуции нет. «Обмен» — sampled, не мгновенный пик.' : 'Pre-HTTP: OS/network/application heap and reserves; exact attribution unavailable. Traffic is sampled, not an exact peak.', 12, '#655c50');
  writeFileSync(resolve(output, `readme-memory-${lang}.svg`), svg + '</g></svg>\n');

  const speedTitle = ru ? 'Отзывчивость и средняя загрузка CPU' : 'Responsiveness and average CPU load';
  svg = begin(468, speedTitle, 'Same Wi-Fi workload; API p95 is round-trip latency, not server-only processing. CPU is normalized across available cores.');
  svg += text(24, 32, speedTitle, 22);
  svg += text(24, 56, ru ? 'API 1/с · WS 256 Б × 2/с на соединение · SSE 1/с · 45 с · cooperative TX' : 'API 1 Hz · WS 256 B × 2 Hz per connection · SSE 1 Hz · 45 s · cooperative TX', 14);
  svg += text(190, 90, ru ? 'CPU во время обмена, %' : 'Active CPU, %', 16);
  svg += text(670, 90, ru ? 'API p95, мс (Wi-Fi round trip)' : 'API p95, ms (Wi-Fi round trip)', 16);
  for (const [x, max] of [[190, 12], [670, 60]]) for (let i = 0; i <= 4; ++i) {
    svg += text(x + i * 80, 115, number(max * i / 4, lang, 0), 12, '#71675c', 'text-anchor="middle"');
    svg += `<path d="M${x + i * 80} 124V408" stroke="#e1dace" stroke-width="1"/>`;
  }
  for (const [index, row] of data.rows.entries()) {
    const y = 133 + index * 42 + (index >= 3 ? 14 : 0), color = row.board === 'c3' ? '#597c9d' : '#538b72';
    svg += text(24, y + 18, profile(row, lang) + (row.strict_pass ? '' : ' †'));
    for (const [x, value, max, suffix] of [[190, row.active_cpu_percent, 12, '%'], [670, row.api.latency.p95, 60, '']]) {
      const size = 320 * value / max;
      svg += rect(x, y, size, 24, color) + text(x + size + 7, y + 18, number(value, lang, 2) + suffix, 13);
    }
  }
  svg += text(24, 432, ru ? '† Максимальный S3: API и SSE без потерь; 1 отказ WS из 540 (0,19%). Это предел теста с замечанием.' : '† Maximum S3: API and SSE completed; 1 WS enqueue rejected out of 540 (0.19%). Qualified result.', 13);
  svg += text(24, 454, ru ? 'CPU: среднее по доступным ядрам. API p95 включает сеть и клиент, не только обработчик.' : 'CPU: average across available cores. API p95 includes networking and client time, not just the handler.', 12, '#655c50');
  writeFileSync(resolve(output, `readme-performance-${lang}.svg`), svg + '</g></svg>\n');

  const table = [ru ? '| Плата / профиль | Heap старта, КиБ | Свободно при обмене, КиБ / % SRAM | CPU простой → обмен, % | API p95, мс | Результат |' :
    '| Board / profile | Startup heap, KiB | Free under load, KiB / % SRAM | CPU idle → active, % | API p95, ms | Result |',
    '|---|---:|---:|---:|---:|---|'];
  for (const row of data.rows) table.push(`| ${profile(row, lang)} | ${number(row.startup_heap_bytes / 1024, lang)} | ${number(row.minimum_sampled_free_bytes / 1024, lang)} / ${number(pct(row.minimum_sampled_free_bytes, row.physical_bytes), lang)}% | ${number(row.idle_cpu_percent, lang, 2)} → ${number(row.active_cpu_percent, lang, 2)} | ${number(row.api.latency.p95, lang)} | ${row.strict_pass ? 'OK' : (ru ? '1 отказ WS †' : '1 WS rejection †')} |`);
  const maxRows = data.rows.filter(v => v.profile === 3);
  const changes = maxRows.map(v => pct(v.minimum_sampled_free_bytes - data.rows.find(m => m.board === v.board && m.profile === 1).minimum_sampled_free_bytes,
    data.rows.find(m => m.board === v.board && m.profile === 1).minimum_sampled_free_bytes));
  const findings = ru ? `От Мини к максимальному профилю свободный heap уменьшился на **${number(-changes[0], lang)}% у C3 / ${number(-changes[1], lang)}% у S3**.\nУ S3 максимальный тест завершился с **одним отказом WS из 540 (0,19%)**; API и четыре SSE работали.\nПовтор временного SSE \`Busy\` устранил пропуски публикаций в стенде; максимальная пауза SSE достигала 1,73 с.\nКороткий прогон не доказывает длительную устойчивость или гарантированную задержку.` :
    `From Mini to the maximum profile, free heap fell by **${number(-changes[0], lang)}% on C3 / ${number(-changes[1], lang)}% on S3**.\nMaximum S3 completed with **one WS enqueue rejection out of 540 (0.19%)**; API and all four SSE streams worked.\nRetrying transient SSE \`Busy\` removed skipped publications in the fixture; the largest SSE gap reached 1.73 s.\nA short run does not prove soak stability or guaranteed latency.`;
  const file = resolve(root, lang === 'ru' ? 'Readme.ru.md' : 'Readme.md');
  let readme = readFileSync(file, 'utf8');
  for (const [marker, value] of [['PROFILE_TABLE', table.join('\n')], ['PROFILE_FINDINGS', findings]]) {
    const tag = marker + '_' + lang.toUpperCase();
    readme = readme.replace(new RegExp(`<!-- ${tag} -->[\\s\\S]*?<!-- /${tag} -->`), `<!-- ${tag} -->\n\n${value}\n\n<!-- /${tag} -->`);
  }
  writeFileSync(file, readme);
}
console.log('Updated four SVG charts and paired README result blocks from sanitized observations.');

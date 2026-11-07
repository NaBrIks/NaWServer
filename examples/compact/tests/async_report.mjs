// SPDX-License-Identifier: MIT
// Charts and paired README excerpts from sanitized on-device observations.
import {readFileSync, writeFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const data = JSON.parse(readFileSync(resolve(root, 'examples/compact/ASYNC_RESULTS.json'), 'utf8'));
const escape = value => String(value).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
const num = (x, lang, digits = 1) => x.toFixed(digits).replace('.', lang === 'ru' ? ',' : '.');
const text = (x, y, value, size = 14, color = '#352f28', extra = '') => `<text x="${x}" y="${y}" font-size="${size}" fill="${color}" ${extra}>${escape(value)}</text>`;
const rect = (x, y, width, height, color) => `<rect x="${x}" y="${y}" width="${width}" height="${height}" fill="${color}"/>`;
const rate = row => 100 * (row.application.misses + row.application.skipped) / row.application.periods;
const get = (board, scenario) => data.rows.find(row => row.board === board && row.scenario === scenario);
const selected = data.rows.filter(row => ['web_only', 'compute80_web', 'priority6_web', 'core1_priority6_web'].includes(row.scenario));

for (const lang of ['ru', 'en']) {
  const ru = lang === 'ru';
  const labels = ru ? {quiet: 'Тихий', compute_only: '80%, p1, CPU0 без веба', priority6_only: '80%, p6, CPU0 без веба', web_only: 'Веб без вычислений', compute50_web: '50%, p1, CPU0 + веб', compute80_web: '80%, p1, CPU0 + веб', priority6_web: '80%, p6, CPU0 + веб', core1_priority6_only: '80%, p6, CPU1 без веба', core1_priority6_web: '80%, p6, CPU1 + веб'} :
    {quiet: 'Quiet', compute_only: '80%, p1, CPU0, no web', priority6_only: '80%, p6, CPU0, no web', web_only: 'Web, no computation', compute50_web: '50%, p1, CPU0 + web', compute80_web: '80%, p1, CPU0 + web', priority6_web: '80%, p6, CPU0 + web', core1_priority6_only: '80%, p6, CPU1, no web', core1_priority6_web: '80%, p6, CPU1 + web'};
  const title = ru ? 'Основная программа и веб: влияние приоритетов и ядра' : 'Application and web: effects of priority and core placement';
  let svg = `<svg xmlns="http://www.w3.org/2000/svg" width="1160" height="635" viewBox="0 0 1160 635" role="img" aria-labelledby="title desc"><title id="title">${escape(title)}</title><desc id="desc">Matched synthetic workload. Deadline miss and skipped-period rates use counter deltas. Start jitter p99 is a histogram upper bound for the configuration epoch. Percent workload is approximate calibrated work, not measured CPU duty.</desc><rect width="1160" height="635" fill="#faf8f3"/><g font-family="Segoe UI,Arial,sans-serif">`;
  svg += text(24, 32, title, 21);
  svg += text(24, 56, ru ? 'Задание каждые 10 мс · 2 WS + 2 SSE + API + файл 64 КиБ · 20 с/режим · отдельная TX-задача' : '10 ms application period · 2 WS + 2 SSE + API + 64 KiB file · 20 s/case · dedicated TX worker', 14);
  svg += rect(24, 72, 12, 12, '#ba6354') + text(43, 83, ru ? 'Опоздавшие задания' : 'Late jobs', 12);
  svg += rect(218, 72, 12, 12, '#d4a14b') + text(237, 83, ru ? 'Пропущенные периоды' : 'Skipped periods', 12);
  svg += text(505, 83, ru ? 'p1/p6 — приоритет программы (6 выше 1); HTTP/WS/TX = 5, SSE = 4' : 'p1/p6: app priority (6 above 1); HTTP/WS/TX = 5, SSE = 4', 12);
  const plots = [
    {x: 345, width: 190, max: Math.max(10, Math.ceil(Math.max(...selected.map(rate)) / 10) * 10), title: ru ? 'Нарушено сроков, %' : 'Deadline violations, %'},
    {x: 655, width: 175, max: Math.max(100, Math.ceil(Math.max(...selected.map(r => r.web.api_p95_ms)) / 20) * 20), title: ru ? 'API p95, мс' : 'API p95, ms'},
    {x: 945, width: 165, max: Math.max(80, Math.ceil(Math.max(...selected.map(r => r.web.file_kib_per_second)) / 20) * 20), title: ru ? 'Файл, КиБ/с' : 'File, KiB/s'}
  ];
  for (const plot of plots) {
    svg += text(plot.x, 111, plot.title, 14);
    for (let i = 0; i <= 4; ++i) {
      const x = plot.x + i * plot.width / 4;
      const value = plot.max * i / 4;
      svg += text(x, 136, num(value, lang, Number.isInteger(value) ? 0 : 1), 11, '#71675c', 'text-anchor="middle"');
      svg += `<path d="M${x} 146V526" stroke="#e1dace"/>`;
    }
  }
  selected.forEach((row, index) => {
    const y = 160 + index * 50 + (row.board === 's3' ? 12 : 0);
    svg += text(24, y + 17, row.board.toUpperCase() + ' · ' + labels[row.scenario], 14, row.board === 'c3' ? '#352f28' : '#37664f');
    svg += text(24, y + 35, ru ? `Опоздание старта p99 ≤ ${num(row.application.late_p99_bound_us / 1000, lang, 2)} мс` : `Start jitter p99 ≤ ${num(row.application.late_p99_bound_us / 1000, lang, 2)} ms`, 11, '#655c50');
    const late = 100 * row.application.misses / row.application.periods, skipped = 100 * row.application.skipped / row.application.periods;
    const p = plots[0], first = p.width * late / p.max;
    svg += rect(p.x, y, first, 22, '#ba6354') + rect(p.x + first, y, p.width * skipped / p.max, 22, '#d4a14b');
    svg += text(p.x + p.width * rate(row) / p.max + 6, y + 16, num(rate(row), lang, 2), 12);
    for (const [plot, value, color] of [[plots[1], row.web.api_p95_ms, '#597c9d'], [plots[2], row.web.file_kib_per_second, '#538b72']]) {
      const width = plot.width * value / plot.max;
      svg += rect(plot.x, y, width, 22, color) + text(plot.x + width + 6, y + 16, num(value, lang), 12);
    }
  });
  svg += text(24, 559, ru ? '50/80% — приблизительный объём вычислений одного ядра. CPU0/CPU1 выполняют одинаковое число итераций.' : '50/80% is approximate calibrated work on one core. CPU0/CPU1 execute the same iteration count.', 12);
  svg += text(24, 582, ru ? 'Jitter p99 — граница корзины за epoch; нарушения сроков — дельты счётчиков. API p95 включает Wi-Fi и клиент.' : 'Jitter p99 is an epoch histogram bound; deadline rates use counter deltas. API p95 includes Wi-Fi and client time.', 12);
  svg += text(24, 605, ru ? 'Ошибки передачи, максимумы и сравнение с программой без веба — в отчёте. Короткий опыт не даёт realtime-гарантий.' : 'Transfer errors, maxima and matched no-web application cases are in the report. This short run gives no real-time guarantees.', 12);
  writeFileSync(resolve(root, `docs/images/async-workload-${lang}.svg`), svg + '</g></svg>\n');

  const low = get('c3', 'compute80_web'), high = get('c3', 'priority6_web');
  const latencyDelta = (high.web.api_p95_ms / low.web.api_p95_ms - 1) * 100;
  const findings = ru ? `На C3 повышение приоритета программы с 1 до 6 изменило долю нарушенных сроков\nс **${num(rate(low), lang, 2)}% до ${num(rate(high), lang, 2)}% в 20-секундном окне**, а API p95 — с **${num(low.web.api_p95_ms, lang)} до ${num(high.web.api_p95_ms, lang)} мс\n(${latencyDelta >= 0 ? '+' : ''}${num(latencyDelta, lang)}%)**. Так приоритет распределяет отзывчивость между программой и вебом.` :
    `On C3, raising application priority from 1 to 6 changed deadline violations\nfrom **${num(rate(low), lang, 2)}% to ${num(rate(high), lang, 2)}% in the 20-second window**, and API p95 from **${num(low.web.api_p95_ms, lang)} to ${num(high.web.api_p95_ms, lang)} ms\n(${latencyDelta >= 0 ? '+' : ''}${num(latencyDelta, lang)}%)**. Priority distributes responsiveness between application work and web traffic.`;
  const readme = resolve(root, ru ? 'Readme.ru.md' : 'Readme.md');
  const tag = 'ASYNC_OVERVIEW_' + lang.toUpperCase();
  let content = readFileSync(readme, 'utf8');
  const block = `<!-- ${tag} -->\n\n![${title}](docs/images/async-workload-${lang}.svg)\n\n${findings}\n\n<!-- /${tag} -->`;
  if (content.includes('<!-- ' + tag + ' -->')) content = content.replace(new RegExp(`<!-- ${tag} -->[\\s\\S]*?<!-- /${tag} -->`), block);
  else content = content.replace(/!\[[^\]]*\]\(docs\/images\/nawserver-capabilities\.png\)/, block + '\n\n$&');
  writeFileSync(readme, content);

  if (ru) {
    const table = ['| Плата / режим | CPU, % | Периодов | Опоздало / пропущено | Нарушено, % | Старт p99 ≤ / max, мс | API p95, мс | Файл, КиБ/с |', '|---|---:|---:|---:|---:|---:|---:|---:|'];
    const transfers = ['| Плата / режим с вебом | API OK / ошибки | Файлы OK / ошибки | WS получено / отправлено | SSE событий | SSE max пауза, с |', '|---|---:|---:|---:|---|---:|'];
    for (const row of data.rows) {
      table.push(`| ${row.board.toUpperCase()} · ${labels[row.scenario]} | ${num(row.cpu_percent, lang)} | ${row.application.periods} | ${row.application.misses} / ${row.application.skipped} | ${num(rate(row), lang, 2)} | ${num(row.application.late_p99_bound_us / 1000, lang, 2)} / ${num(row.application.late_max_us / 1000, lang, 2)} | ${row.web.active ? num(row.web.api_p95_ms, lang) : '—'} | ${row.web.active ? num(row.web.file_kib_per_second, lang) : '—'} |`);
      if (row.web.active) transfers.push(`| ${row.board.toUpperCase()} · ${labels[row.scenario]} | ${row.web.api_ok} / ${row.web.api_errors} | ${row.web.file_ok} / ${row.web.file_errors} | ${row.web.ws_echoes} / ${row.web.ws_sent} | ${row.web.sse_events.join(' / ')} | ${num(row.web.sse_max_gap_ms / 1000, lang, 2)} |`);
    }
    const report = resolve(root, 'docs/readme_async_workload_ru.md');
    let content = readFileSync(report, 'utf8');
    content = content.replace(/<!-- ASYNC_RESULTS -->[\s\S]*?<!-- \/ASYNC_RESULTS -->/, '<!-- ASYNC_RESULTS -->\n\n![Основная программа и веб](images/async-workload-ru.svg)\n\n' + findings + '\n\n' + table.join('\n') + '\n\n' + transfers.join('\n') + '\n\n<!-- /ASYNC_RESULTS -->');
    writeFileSync(report, content);
  }
}
console.log('Wrote paired asynchronous-workload charts, overview blocks and the detailed report.');

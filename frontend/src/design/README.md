# Происхождение оформления

19.10.2026 перенесены из UI.TechnicalShell (`src/TechnicalShell/wwwroot`):

- `themes.css` — палитры и токены `--lu-*`, без изменения цветов.
- `ui-tooltip.js`, `ui-tooltip.css` — общие подсказки с hover/focus, Escape,
  aria-describedby, привязкой к элементу и обходом большого курсора.
- `icons/tabler-icons.css`, `icons/tabler-icons.woff`, `icons/LICENSE.txt` —
  согласованная локальная карта Tabler Icons; MIT / Paweł Kuna.

Лицензия иконок также включена в production resources как
`/licenses/Tabler-Icons.txt`. Общая лицензия репозитория не изменена.
Vue-оболочка адаптирует правила README/REQUIREMENTS/AGENTS набора, а не переносит
его Blazor runtime или предметную логику. Компоненты Icon, InfoButton и
TransportDiagram принадлежат Showcase. Общая компоновка — `src/style.css`,
оформление пульта — `features/simulator.css`; в inline bindings остаются только
значения данных (например, фактический цвет RGB).

Аппаратная проверка HTTP C3 выявила отсутствие crypto.randomUUID вне secure
context. В локальной копии ui-tooltip.js добавлен fallback для ARIA-id; это не
идентификатор сессии или механизм авторизации. Исходный TechnicalShell не менялся.

В S3 пример оформление скопировано из examples/showcase 19.10.2026 по прямому запросу владельца. Файлы самостоятельные; подключений к sibling при сборке нет. В S3 копии tooltip получил pointer-events:none для устранения перехвата кликов меню.

20.10.2026: help-circle.svg и device-desktop.svg взяты из официального tabler/tabler-icons (icons/outline), MIT. SVG добавлены локально, поскольку в перенесённой CSS-карте этих имён нет. База шрифта 14px, line-height 1.35; Nano/Mini/Norm меняют отступы 3/5/8px, как в TechnicalShell.

Переключатель оформления восстановлен по AppShell TechnicalShell / оболочки других проектов владельца: одна кнопка с текущей темой и плотностью, общее меню, локализованные подписи, штатные иконки и выбранные пункты. Отдельные select в шапке удалены.

20.10.2026: актуальная S3 версия стала единственным общим frontend в корне репозитория. Исторические записи выше о независимой копии описывают прежнюю структуру. Профили и страницы платы находятся в examples/<project>/frontend; общие файлы больше не копируются.

# URL decoder: сравнение четырёх вариантов на C3

Сравниваются Direct, Lookup, std::from_chars и Spans по общему
[length-aware контракту](../tools/url_decode/CONTRACT.md).
Алгоритмы, преимущества и ограничения описаны в
[руководстве интеграции](readme_url_decode_integration_ru.md).

## Измерения

- [ASCII, формы, path, ошибки и NUL: таблицы](../tools/url_decode/results/tables.md).
- [Русский, китайский, японский и emoji: таблицы](../tools/url_decode/results/unicode/tables.md).
- [Unicode: сводные CSV](../tools/url_decode/results/unicode/summary.csv).
- [Unicode: стек функций](../tools/url_decode/results/unicode/stack-frames.csv).
- [Unicode: аллокации и память](../tools/url_decode/results/unicode/memory.csv).

Это сохранённые измерения четырёх кандидатов на C3 от 19 октября 2026,
а не новый запуск на плате. Таблицы пересобраны из записанных данных;
состав сравнений сокращён по указанию владельца. Метаданные прежнего полного
benchmark-бинарника удалены, поскольку они не описывают текущую сборку.

C++17, GCC14.2, ESP-IDF5.4, CPU160 MHz, `-Os`, без LTO.
Три раунда: warm/owning по 27 batches на ячейку, pressure по 45 одиночных вызовов.
Показаны gross cycles/op: медиана [min; max]. В gross входят копирование входа,
вызов и потребление результата. Отдельные overhead строки доступны в CSV;
они не превращают разность в точную стоимость hex kernel.

Unicode fixtures содержат по 1024 байта UTF-8 и 3072 байта percent-encoded ввода.
Декодеры работают с байтами, сохраняют NUL, не валидируют и не нормализуют Unicode.
Корректность проверяется отдельно от скорости: 753597 checks, 0 failures.
Стек — собственный compiler frame функции, не пик полного call chain.
Сам decoder не выделяет память; owning режим отдельно включает std::string.
Pressure — искусственное давление на cache, не гарантированно холодный cache.

Lookup выбран для скорости, from_chars — для стандартного библиотечного API.
Производительность полного HTTP handler включает дополнительные расходы.
Текущие production/hardware результаты находятся в руководстве интеграции.

# Проверка общего frontend — 20.10.2026

Этот отчёт фиксирует первоначальное выделение общего frontend до изменения
CBOR. Числовые ID ниже относятся только к той проверке и больше не применяются.
Актуальный формат и проверки: [CBOR](readme_cbor_requirements_ru.md),
[миграция](readme_cbor_verification_ru.md).

Общая основа — текущий S3 frontend со всеми изменениями этой сессии. Исходники
обеих копий frontend/src заменены единственным корневым frontend/src. Локальные
профили выбирают storage key, поддержку admission и дополнительные страницы.
LCD/GT911 компонент, его типы, тексты справок и CSS находятся только в S3 проекте.

## Сборка и упаковка

- `npm ci --no-audit --no-fund` из корня — PASS. Один workspace lock.
- `npm run build` — PASS: vue-tsc + Vite для showcase и touch_lcd7.
- `npm test` — 6/6 PASS: отсутствие копий src, общие стили, исключение LCD
  extension из C3 bundle, разные storage keys/admission profiles, совпадение
  числовых ID общих CBOR полей и GPIO/SoC из состояния устройства.
- Go packer S3 и .NET Showcase.Assets — PASS. Simulator chunks исключены из
  встраиваемых ресурсов; отдельный LCD chunk S3 включён.
- .NET solution Showcase — PASS; работающий Host был перезапущен с проверкой
  PID, пути и времени старта для обновления маршрутов статических ресурсов.
- Showcase.Tests с `--url http://127.0.0.1:8788` — 106 PASS, включая модель,
  HTTP/auth/CBOR, бинарный WS, SSE и identity/gzip проверку ресурсов.
- `pio run -e c3 -j 8` — SUCCESS: RAM 61848 B, flash sections 1073844 B.
- `pio run -e s3 -j 8` — SUCCESS: RAM 72688 B, flash sections 1344972 B.

Первый C3 запуск использовал устаревший кэш со ссылками на прежнее расположение
src/handler.cpp и завершился ошибкой. Штатный `pio ... -t clean` и повторная
сборка устранили проблему; исходники ядра для этого не изменялись.

## Реальный браузер

В Codex in-app browser проверены оба локальных адреса:

- S3: http://127.0.0.1:8790 — HTTP/WS/SSE, binary CBOR echo, меню и группы;
  отдельная LCD страница открывается как асинхронное расширение.
- C3: http://127.0.0.1:8788 — те же общие части; пункта LCD нет, отсутствующий
  admission endpoint не вызывается. Данные SoC приходят из API.
- На обеих целях: порядок тем, Dark/Mini, сохранение после reload,
  возврат Cream/Nano, переключение RU/EN и навигация.

Это проверка локальных эмуляторов и сборок. Новые образы на платы не
записывались; аппаратный runtime после этого рефакторинга не проверен.

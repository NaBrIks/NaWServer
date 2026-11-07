# Проверки нового middleware, 18 октября 2026

Актуализация после следующего этапа: версия с raw/copy body уже прошита и проверена
на новой C3; [аппаратный отчёт](readme_request_body_verification_ru.md).
Прежние замечания о непрошитой версии ниже относятся к предыдущему этапу.


Это отдельный этап после первоначального выделения. Новый код на C3 пока не записан;
предыдущий boot/HTTP/WS smoke остаётся доказательством только прежней прошивки.

## Логика цепочки на компьютере

PASS: 35 проверок, C++17, `-Wall -Wextra -Werror -pedantic`. Проверены все три фасада,
before/after, пустая цепочка, короткий отказ, повторный next без повторного endpoint,
права, пропущенный access middleware, неверный порядок, сброс identity, const middleware
и providers, передача challenge и очистка старого challenge при повторном запуске.
Во время выполнения проверенных цепочек число C++ allocations не изменилось.
Это проверка pipeline, а не heap-профиль HTTPD, Basic или всего приложения.

Использован task-local Zig 0.14.1 как C++ compiler; архив сверён с SHA-256 официального
download/index.json. Расположение: игнорируемый .agent-work/host-cpp исходного workspace.
MSVC был обнаружен, но оказался неполным; глобальные инструменты не менялись.
Firmware ни от одного компилятора ПК не зависит: его собирает ESP-IDF GCC.

Источники: tests/pipeline_test.cpp, tests/CMakeLists.txt. Результат local/test-pipeline.log.
Для обычного host toolchain: cmake -S tests -B local/host-tests, затем cmake --build
local/host-tests и ctest --test-dir local/host-tests --output-on-failure. Для многоконфигурационного
генератора указать одну конфигурацию при build и ctest. Host tests необязательны для firmware.

## Сборки ESP32

Изолированный PlatformIO Core, espressif32@6.10.0, ESP-IDF 5.4.0, GCC 14.2.0+20241119,
C++17, `-j 2`. Версии глобальных SDK не менялись. При изменении component source list
проверка PlatformIO требовала обновления CMake configure, иначе оставалась ссылка на
удалённый web_server_container.cpp. Итоговая сборка использует новый список исходников.

После последнего уточнения provider contract (authenticate + challenge) повторены:

| Пример / target | Результат | Что проверяет |
|---|---|---|
| minimal / C3 | PASS | makeChain, const-capable provider, статическая таблица, публичный WS |
| compile_check / C3 | PASS | Все три формы chain и permission policy |
| compile_check / C6 | PASS | Компиляция нового API |
| compile_check / ESP32 | PASS | Компиляция нового API на Xtensa |
| compile_check / S2 | PASS | Компиляция нового API |
| compile_check / S3 | PASS | Компиляция нового API |

Итоговый minimal/C3: статический RAM 29264 байт, flash 865712 байт.
Это отчёт линкера конкретного примера, не пик heap и не измерение экономии фич.
Логи: local/build-pipeline-c3.log, local/build-pipeline-matrix.log.

Showcase/C3 ранее успешно собрался после минимальной миграции auth integration;
лог local/build-pipeline-showcase-c3.log. Эта сборка предшествует последнему уточнению
challenge и текущей работе отдельного демо-чата. Текущее состояние showcase здесь
повторно не проверялось. Его файлы оставлены демо-чату, generated assets здесь
после изменения фрагмента web/index.html не перепаковывались.

## Ограничения

Без новой прошивки нет подтверждения boot, получения IP и HTTP/WS поведения этого
изменения на устройстве. Новые auth/pipeline не проверены сетевыми тестами или под нагрузкой.
Матрица сборки не подтверждает runtime поддержку всех вариантов ESP32.

Pipeline пока исполняется после чтения ограниченного body. Публичные static handlers
сохранены; file/source serving ещё не реализован. Динамические роли/хранилища, custom
Kconfig feature profiles и выбранные транспортные лимиты не реализованы. В частности,
WS пока сохраняет старые void send и drop при overflow. SSE исключён, authenticated WS
не поддерживается. Очередь при stop и конкурентный shutdown всё ещё требуют проверки.
HTTPS/WSS записаны в идеи следующего этапа; TLS код и измерения не добавлены.

Основной TPMS не редактировался и не переключался. COM5, flash, NVS, настройки сети,
роутера и системные инструменты в этом этапе не менялись.

## Go smoke и прежняя прошивка C3

Исторический tools/smoke_test.py заменён tools/smoke, стандартная библиотека Go,
без новых внешних зависимостей. Пять групп локальных тестов codec/IO/target/HTTP прошли,
в том числе проверка Basic challenge и скрытие случайно введённых credentials в ошибке
парсинга target. Windows executable собран с -buildvcs=false. Это отключает ненужную
запись Git metadata в бинарник и не меняет Git configuration.

Прежний сервер сначала идентифицирован по корневой странице minimal. На этой уже
работающей прошивке новый Go-клиент успешно проверил HTTP/forms/Basic/DELETE,
частичный POST, body limit, WS text/binary 0..4096, пять reconnect и последующий HTTP.
Отказы Basic дополнительно проверены на WWW-Authenticate. Лог local/test-go-smoke-baseline.log.
Проверка не записывала flash/NVS и не захватывала COM5; она подтверждает инструмент
и прежний baseline, а не новый middleware на плате. Нагрузочный тест не запускался.

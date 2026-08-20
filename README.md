# json-lib

`json-lib` — самостоятельная библиотека **ISO C11** для разбора JSON object-документов, безопасной навигации по плоскому массиву токенов, типизированного извлечения значений и атомарной записи JSON-файлов. Она не зависит от `benchmark-framework`, `bignum-lib` или сторонних JSON-библиотек; результатом `make dist` является статическая библиотека `libjson_lib.a`.

Публичный контракт намеренно строгий: каждая библиотечная функция возвращает именованный `json_status_t`, а полезные данные передаются только через output-параметры. Предикаты используют `json_boolean_t`, выделенные строки освобождаются `json_memory_free()`, а файловая публикация выполняется через явную writer-transaction.

> **Граница документа.** Библиотека принимает JSON, корнем которого является object. Общий JSON допускает root array, primitive и string, однако `json_document_parse_text()` и `json_document_load_file()` намеренно возвращают для них `JSON_STATUS_SYNTAX_ERROR`: API предназначен для конфигураций, manifests и structured reports с именованным root object.

## Содержание

| Раздел | Назначение |
|---|---|
| [Возможности](#возможности) | Краткий обзор реализованных свойств |
| [Получение и зависимости](#получение-и-зависимости) | Инструменты для сборки, проверок и benchmark |
| [Модель данных](#модель-данных) | Ownership, токены и потоковая безопасность |
| [Статусы и правила API](#статусы-и-правила-api) | Точный return/output contract |
| [Практические руководства](#практические-руководства) | Пошаговые how-to примеры |
| [Сборка и quality gates](#сборка-и-quality-gates) | Все доступные Makefile цели |
| [Бенчмарки](#бенчмарки) | Protocol, параметры и ограничения `perf` |
| [Распространение](#распространение) | Состав distribution и linking |

## Возможности

| Возможность | Реализация и контракт |
|---|---|
| Строгий JSON parser | Recursive-descent parser проверяет object/array grammar, string escapes, numbers, literals, separators и отсутствие trailing bytes. |
| Flat token representation | Исходный текст хранится один раз; каждый `json_token_t` содержит text range и `parent_index`, без heap-дерева. |
| Object и array navigation | `json_object_get()`, `json_array_size()` и `json_array_get()` обходят непосредственных children и корректно пропускают nested subtrees. |
| Типизированные значения | Поддерживаются raw text copy, `uint64_t`, конечный `double` и exact `true`/`false`; неявных преобразований нет. |
| Atomic writer | Временный файл создаётся рядом с target, затем `fflush()`, `fsync()` и `rename()` публикуют artifact. |
| Status-only API | Библиотечные функции не возвращают `void`, `int`, `char`, pointers или untyped boolean как результат операции. |
| Reentrancy | Нет global mutable state; независимые document/writer objects пригодны для параллельного использования. |

## Получение и зависимости

Клонируйте репозиторий и перейдите в рабочую директорию:

```bash
git clone https://github.com/kirill-bayborodov/json-lib.git
cd json-lib
```

| Зависимость | Для чего необходима |
|---|---|
| `make` | Единая точка запуска build, test, lint, distribution и benchmark targets |
| `gcc` с C11 | Строгая компиляция `-std=c11 -Wall -Wextra -Werror -pedantic` |
| `ar`, `ranlib` | Формирование static archive при `make install` и `make dist` |
| `cppcheck` | Static analysis в `make lint` |
| `valgrind` | Helgrind target для будущих `_mt` tests |
| `perf` | Sampling profile и repeated software-counter measurements |

На cloud/container host `perf record` может создать data-файл без samples из-за ограничений ядра или отсутствия разрешённого PMU. Это не мешает сборке и прямому запуску benchmark binary, но делает `perf report` непригодным для анализа горячих функций. См. [ограничения среды](#ограничения-среды-для-perf).

## Модель данных

### Жизненный цикл `json_document_t`

`json_document_t` владеет копией input JSON и массивом токенов. Пользователь всегда инициализирует объект перед первым использованием и освобождает его только публичной функцией destroy.

```text
json_document_init
        │
        ├── json_document_parse_text ──► navigation/conversion ──► json_document_destroy
        │
        └── json_document_load_file ──► navigation/conversion ──► json_document_destroy
```

Повторный `json_document_parse_text()` или `json_document_load_file()` для того же инициализированного объекта сначала освобождает прежнее состояние. После неудачной parse/load операции объект возвращается в пустое состояние и допускает destroy либо новую загрузку.

| Поле `json_document_t` | Ownership и значение |
|---|---|
| `text` | Owned NUL-terminated копия JSON source. Пользователь не изменяет и не освобождает её напрямую. |
| `tokens` | Owned префиксный `json_token_t` array. Индексы public API относятся к этому array. |
| `text_size` | Размер source без завершающего NUL; доступен только для read-only диагностики. |
| `token_count` | Число полностью инициализированных tokens. |

### Токены и навигация

Parser добавляет токены в префиксном порядке. Контейнер всегда находится раньше всех его descendants. Это позволяет библиотеке искать следующий sibling, пропуская целое nested subtree без построения pointer-tree.

Рассмотрим документ:

```json
{
  "build": {
    "number": 42,
    "targets": ["test", "dist"]
  }
}
```

| Логический token | Тип | Родитель | Значение `child_count` |
|---|---|---:|---:|
| root | `JSON_TOKEN_OBJECT` | `-1` | 2 — key `build` и его value |
| `build` key | `JSON_TOKEN_STRING` | root | 0 |
| `build` value | `JSON_TOKEN_OBJECT` | root | 4 — две key/value пары |
| `number` key | `JSON_TOKEN_STRING` | build object | 0 |
| `42` | `JSON_TOKEN_PRIMITIVE` | build object | 0 |
| `targets` key | `JSON_TOKEN_STRING` | build object | 0 |
| targets value | `JSON_TOKEN_ARRAY` | build object | 2 |

Object `child_count` включает key и value tokens, поэтому public code не читает это поле как число object fields. Для object navigation предназначен `json_object_get()`. Для array `child_count` уже равен числу элементов и отражается через `json_array_size()`.

### Потоковая безопасность

В реализации нет изменяемых global/static объектов. Два потока могут независимо parse/destroy разные `json_document_t` или писать разные `json_writer_t`. Один объект нельзя одновременно передавать в `json_document_parse_text()`, `json_document_destroy()` или writer operations без внешнего mutex: эти операции меняют owned pointers и state.

## Статусы и правила API

### `json_status_t`

| Статус | Значение | Когда возвращается | Состояние output-параметров |
|---|---:|---|---|
| `JSON_STATUS_SUCCESS` | 0 | Операция завершила документированный алгоритм | Все документированные outputs записаны |
| `JSON_STATUS_ARGUMENT_ERROR` | 1 | `NULL`, неинициализированный/разрушенный document, неверный token index либо type | Не читать output |
| `JSON_STATUS_NOT_FOUND` | 2 | Нет object key или array position | Не читать output |
| `JSON_STATUS_SYNTAX_ERROR` | 3 | Некорректная JSON grammar либо literal не соответствует conversion type | Не читать output |
| `JSON_STATUS_ALLOCATION_ERROR` | 4 | Не удалось выделить source, token array, raw-text copy или writer path | Не читать output |
| `JSON_STATUS_CAPACITY_ERROR` | 5 | Требуемая token/path allocation превышает безопасный `size_t` предел | Не читать output |
| `JSON_STATUS_IO_ERROR` | 6 | Ошибка open/read/write/flush/fsync/close/rename/unlink | Не читать output |

### Общий алгоритм обработки статуса

Публичная функция всегда проверяется до чтения output. Следующий код показывает каноническую форму приложения:

```c
#include "json_lib.h"

int read_build_number(const char *source, uint64_t *build_number)
{
    json_document_t document;
    json_status_t status;
    size_t value_index;

    status = json_document_init(&document);
    if (status == JSON_STATUS_SUCCESS) {
        status = json_document_parse_text(source, &document, NULL, 0U);
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_object_get(&document, 0U, "build", &value_index);
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_token_to_u64(&document, value_index, build_number);
    }

    (void)json_document_destroy(&document);
    return status == JSON_STATUS_SUCCESS ? 0 : 1;
}
```

Внешняя функция примера возвращает `int`, потому что это код приложения. Сама `json-lib` не использует такой return contract.

### Поддерживаемая grammar и ограничения

Поддерживаются JSON object, array, string, number, `true`, `false` и `null`. Строки проверяются на допустимые escapes `\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t` и `\uXXXX`; неэкранированные control bytes запрещены. Числа проверяются по JSON grammar, включая запрет leading zero (`01`), lone decimal point (`1.`) и incomplete exponent (`1e`).

| Вход | `json_document_parse_text()` | Причина |
|---|---|---|
| `{ "value": 42 }` | `JSON_STATUS_SUCCESS` | Root object и корректная grammar |
| `{ "value": [1, {"x": true}] }` | `JSON_STATUS_SUCCESS` | Допускаются nested containers |
| `[1, 2]` | `JSON_STATUS_SYNTAX_ERROR` | JSON валиден, но root не object |
| `{ "value": 01 }` | `JSON_STATUS_SYNTAX_ERROR` | JSON number не допускает leading zero |
| `{ "value": 1, }` | `JSON_STATUS_SYNTAX_ERROR` | Trailing comma запрещена |
| `{ "value": "text` | `JSON_STATUS_SYNTAX_ERROR` | Unterminated string |

## Практические руководства

### How-to: разобрать object и извлечь число

1. Инициализируйте `json_document_t`.
2. Передайте text в `json_document_parse_text()`.
3. Используйте root token index `0U` с `json_object_get()`.
4. Преобразуйте token через `json_token_to_u64()`.
5. Завершите lifecycle через `json_document_destroy()`.

```c
#include "json_lib.h"

int main(void)
{
    json_document_t document;
    json_status_t status;
    size_t token_index;
    uint64_t port = UINT64_C(0);

    status = json_document_init(&document);
    if (status == JSON_STATUS_SUCCESS) {
        status = json_document_parse_text("{\"port\":8080}", &document, NULL, 0U);
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_object_get(&document, 0U, "port", &token_index);
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_token_to_u64(&document, token_index, &port);
    }

    (void)json_document_destroy(&document);
    return status == JSON_STATUS_SUCCESS && port == UINT64_C(8080) ? 0 : 1;
}
```

### How-to: пройти по массиву с вложенными значениями

Получите array token как value object field, запросите число элементов, затем обращайтесь к позициям только через `json_array_get()`. Не вычисляйте индексы вручную: один элемент может быть object или array с произвольным числом вложенных tokens.

```c
json_document_t document;
json_status_t status;
size_t array_index;
size_t count;
size_t position;

status = json_document_init(&document);
if (status == JSON_STATUS_SUCCESS) {
    status = json_document_parse_text(
        "{\"targets\":[\"test\",{\"name\":\"dist\"}]}",
        &document, NULL, 0U);
}
if (status == JSON_STATUS_SUCCESS) {
    status = json_object_get(&document, 0U, "targets", &array_index);
}
if (status == JSON_STATUS_SUCCESS) {
    status = json_array_size(&document, array_index, &count);
}
for (position = 0U; (status == JSON_STATUS_SUCCESS) && (position < count);
    position += 1U) {
    size_t element_index;
    status = json_array_get(&document, array_index, position, &element_index);
    /* Обработайте element_index через json_token_has_type()/json_object_get(). */
}
(void)json_document_destroy(&document);
```

### How-to: скопировать string token и освободить память

`json_token_copy_text()` возвращает raw JSON text token без внешних кавычек. Escape-последовательности остаются кодированными: `"line\\n"` даст две raw bytes `\\` и `n`, а не newline byte. Освобождайте результат только через `json_memory_free()`.

```c
char *raw_name = NULL;
size_t name_index;

status = json_object_get(&document, 0U, "name", &name_index);
if (status == JSON_STATUS_SUCCESS) {
    status = json_token_copy_text(&document, name_index, &raw_name);
}
if (status == JSON_STATUS_SUCCESS) {
    /* Используйте raw_name до освобождения. */
    status = json_memory_free(raw_name);
}
```

### How-to: использовать typed predicates и conversions

Сначала подтвердите type token, когда input schema допускает несколько вариантов. Затем выберите explicit conversion. `json_token_to_boolean()` принимает только exact literals `true` и `false`; `json_token_to_double()` возвращает только finite values; `json_token_to_u64()` отклоняет `-1`, `1.5` и exponent form.

| Требуемое значение | Последовательность API | Пример допустимого raw text |
|---|---|---|
| Строка schema marker | `json_token_has_type(..., JSON_TOKEN_STRING, ...)` → `json_token_equals()` или `json_token_copy_text()` | `release` |
| Беззнаковый счётчик | `json_token_to_u64()` | `42` |
| Коэффициент | `json_token_to_double()` | `0.125`, `-1.0e3` |
| Feature flag | `json_token_to_boolean()` | `true`, `false` |

### How-to: загрузить конфигурацию из файла

`json_document_load_file()` различает filesystem error и JSON grammar error. Передайте optional buffer, когда приложению нужна компактная diagnostic string.

```c
json_document_t config;
char error[160];
json_status_t status;

status = json_document_init(&config);
if (status == JSON_STATUS_SUCCESS) {
    status = json_document_load_file("config.json", &config, error, sizeof(error));
}
if (status == JSON_STATUS_IO_ERROR) {
    /* Файл недоступен или не прочитан полностью. */
}
if (status == JSON_STATUS_SYNTAX_ERROR) {
    /* error содержит краткую parser diagnostic. */
}
(void)json_document_destroy(&config);
```

### How-to: атомарно записать JSON report

Atomic writer предназначен для случая, когда наблюдатель не должен увидеть частично записанный final file. Откройте writer, последовательно запишите trusted punctuation и string values, а затем вызовите commit. При любом промежуточном non-success status вызовите `json_writer_abort()`.

```c
json_writer_t writer = {0};
char error[160];
json_status_t status;

status = json_writer_open("reports/result.json", &writer, error, sizeof(error));
if (status == JSON_STATUS_SUCCESS) {
    status = json_writer_write_raw(&writer, "{\"status\":");
}
if (status == JSON_STATUS_SUCCESS) {
    status = json_writer_write_string(&writer, "passed");
}
if (status == JSON_STATUS_SUCCESS) {
    status = json_writer_write_raw(&writer, ",\"count\":42}");
}
if (status == JSON_STATUS_SUCCESS) {
    status = json_writer_commit(&writer, error, sizeof(error));
} else {
    (void)json_writer_abort(&writer);
}
```

`json_writer_write_raw()` **не валидирует** фрагмент. Используйте его только для library-controlled punctuation и предварительно проверенных literals. Для произвольного текста всегда используйте `json_writer_write_string()`.

## Сборка и quality gates

### Основные цели

| Команда | Результат | Назначение |
|---|---|---|
| `make build SAN=no` | `build/json_lib.o` | Строгая C11 object-сборка без sanitizer runtime |
| `make test` | `bin/test_json_lib`, `bin/test_json_lib_stress` | Deterministic и lifecycle stress suite |
| `make lint` | Нет generated binary | `cppcheck --enable=all` для `src/`, `tests/`, `benchmarks/` |
| `make test_sanitize` | Test suite с ASan/UBSan | Runtime memory и undefined-behaviour validation |
| `make test_helgrind` | Helgrind summary | Race checker для будущих `_mt` test executables |
| `make install` | Header и `libjson_lib.a` в `dist/` | Static library для локального linking |
| `make dist` | Полный distribution directory | Header, static library, README и LICENSE |
| `make clean` | Удаляет `build/`, `bin/`, `dist/` | Очистка build artifacts |

Базовый воспроизводимый quality gate:

```bash
make clean
make build SAN=no
make test
make lint
make test_sanitize
make test_helgrind
make dist
```

Ожидаемая строка test suite:

```text
=== Summary: 0 / 2 failed ===
```

### Состав тестов

| Файл | Уровень | Проверяемые свойства |
|---|---|---|
| `tests/test_json_lib.c` | Deterministic API/integration | Parser grammar, navigation, numeric/boolean/string conversion, capacity growth, file load, atomic writer |
| `tests/test_json_lib_stress.c` | Lifecycle stress | 10 000 независимых init/parse/navigate/convert/destroy transactions |
| `benchmarks/bench_json_lib.c` | Performance executable | Полный parser/navigation/conversion/destroy path и stable result protocol |

## Бенчмарки

### Прямой запуск

Соберите performance binary без sanitizers и выполните smoke scenario:

```bash
make clean
make bin/bench_json_lib SAN=no
./bin/bench_json_lib --documents 10000 --depth 8
```

| Параметр | Default | Значение |
|---|---:|---|
| `--documents N` | `10000` | Число полных parser lifecycle итераций; `N > 0` |
| `--depth N` | `8` | Число nested object levels между root и leaf `metric`; `N > 0` в CLI |

Успешный запуск заканчивается следующими двумя строками; `sink` зависит от `documents`.

```text
benchmark=json_lib documents=10000 depth=8 sink=10000
Benchmark finished.
```

Первая строка предназначена для машинной агрегации. Вторая — обязательный success marker: Makefile проверяет, что он идёт после protocol line.

### Профиль и повторяемые software counters

```bash
make bench \
  REPORT_NAME=parser_smoke \
  JSON_BENCH_DOCUMENTS=10000 \
  JSON_BENCH_DEPTH=8

make bench_stat \
  REPORT_NAME=parser_stat \
  PERF_RUNS=7 \
  PERF_EVENTS=task-clock,context-switches,cpu-migrations,page-faults \
  JSON_BENCH_DOCUMENTS=100000 \
  JSON_BENCH_DEPTH=8
```

| Переменная Makefile | Default | Назначение |
|---|---:|---|
| `REPORT_NAME` | `current` | Базовое имя runtime/profile/stat artifacts в `benchmarks/reports/` |
| `JSON_BENCH_DOCUMENTS` | `10000` | Передаётся benchmark как `--documents` |
| `JSON_BENCH_DEPTH` | `8` | Передаётся benchmark как `--depth` |
| `PERF_RUNS` | `5` | Число повторов `perf stat` |
| `PERF_EVENTS` | `task-clock,context-switches,cpu-migrations,page-faults` | Software events для `perf stat` |
| `PERF` | `/usr/local/bin/perf` | Путь к kernel-compatible `perf` executable |

### Ограничения среды для `perf`

В текущей cloud-среде direct benchmark binary собирается и выводит корректный protocol. Однако `perf record` может создать data-файл без samples; `perf report` тогда печатает сообщение об отсутствии samples. Это не является ошибкой JSON parser или protocol, но означает, что report нельзя использовать для conclusions об instruction-level hotspots.

Для воспроизводимого сравнения на подходящем host фиксируйте compiler, `CONFIG`, documents, depth, CPU affinity, perf events и число повторов. Не сравнивайте один profile без samples с profile, записанным на другом host или другом kernel.

## Распространение

`make dist` формирует минимальный redistributable набор:

```text
dist/
├── include/json_lib.h
├── lib/libjson_lib.a
├── README.md
└── LICENSE
```

Подключите static library к приложению:

```bash
make dist

gcc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I./dist/include \
  your_program.c ./dist/lib/libjson_lib.a \
  -lm -o your_program
```

Для development linking можно использовать `build/json_lib.o`, но `make dist` предпочтительнее: он поставляет public header и static archive как единый артефакт.

## Инвентарь JSON-файлов

В репозитории отсутствуют version-controlled файлы с расширением `.json`. Поэтому отдельная how-to-документация для project-owned JSON manifests в текущем состоянии не требуется. Если в будущем будет добавлен JSON profile, fixture или configuration file, ему должен соответствовать отдельный Markdown-документ с описанием schema, ownership, valid examples и пошаговым how-to.

## Вклад в проект

Изменения public API должны сохранять status-only contract, обновлять Doxygen в `include/json_lib.h`, реализацию и относящиеся test scenarios. До передачи изменения на review выполните:

```bash
make clean
make test
make lint
make test_sanitize
make test_helgrind
make dist
```

Изменения parser grammar, ownership или atomic writer требуют документированной оценки runtime quality gates. Изменения производительности оцениваются только на сопоставимых host/config/workload условиях.

## Лицензия

Проект распространяется по MIT License. Полный текст приведён в [LICENSE](LICENSE).

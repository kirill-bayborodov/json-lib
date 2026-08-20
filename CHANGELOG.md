# Changelog

Все значимые изменения `json-lib` документируются в этом файле. Формат основан на принципах [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/), а номера версий следуют [Semantic Versioning 2.0.0](https://semver.org/lang/ru/).

> **Правило источника.** Release-запись формируется из commit history, аннотированного Git-тега, результатов CI/QG и, при наличии, issue/PR references. Неподтверждённые результаты измерений или планы будущих версий в changelog не включаются.

## [1.0.0] — 2026-08-20

**Тег:** [`v1.0.0`](https://github.com/kirill-bayborodov/json-lib/tree/v1.0.0)

**Commit:** [`38a21bc`](https://github.com/kirill-bayborodov/json-lib/commit/38a21bc86f03589cd408a98d07ff4648208d2504)

**Статус поставки:** initial standalone release.

### Added

Добавлена самостоятельная библиотека ISO C11 `json-lib` с public header `include/json_lib.h` и статическим distribution artifact `libjson_lib.a`. Public API содержит 19 функций `json_*` для lifecycle document, object/array navigation, raw-token copy, строгих conversions `uint64_t`/`double`/`boolean`, memory ownership и atomic JSON writer transaction.

Добавлен flat prefix token model. `json_document_t` владеет неизменяемой копией source и массивом `json_token_t`, каждый из которых содержит byte range, category, direct child count и parent index. Навигация по object и array не требует pointer-tree и пропускает complete nested subtrees по token index.

Добавлен documented recursive-descent parser с проверкой root-object contract, separators, trailing input, nested arrays/objects, JSON literals, string escapes и JSON number grammar. Parser повторяет bounded pass с увеличением token capacity и возвращает разделённые статусы syntax, allocation, capacity и I/O ошибок.

Добавлен atomic writer: temporary file создаётся рядом с target, safe string encoder экранирует quotes, backslashes и control bytes, а commit выполняет `fflush`, `fsync`, `fclose` и `rename`.

Добавлены deterministic API/integration tests и 10 000-итерационный lifecycle stress test. Детерминированная часть покрывает parse/navigation/conversion, malformed JSON, capacity growth, file loading и atomic writer publication.

Добавлен параметризованный C11 benchmark `bench_json_lib` с `--documents` и `--depth`. Успешный запуск завершает machine-readable contract в фиксированном порядке:

```text
benchmark=json_lib documents=<N> depth=<N> sink=<N>
Benchmark finished.
```

Добавлены Makefile цели `build`, `lint`, `test`, `test_sanitize`, `test_helgrind`, `bench`, `bench_stat`, `install` и `dist`. Benchmark targets принудительно изолированы от sanitizer objects и link flags.

Добавлены подробные Doxygen contracts и inline field comments во всех C/header source files. README расширен до руководства по архитектуре, ownership, статусам, grammar, how-to usage, QG, benchmark и distribution linking.

### Quality gates

| Проверка | Результат, зафиксированный аннотацией `v1.0.0` |
|---|---|
| Strict C11 build | Пройдена с `-std=c11 -Wall -Wextra -Werror -pedantic` |
| Deterministic и lifecycle tests | Пройдены; lifecycle test выполняет 10 000 независимых транзакций |
| Static analysis | cppcheck пройден для `src/`, `tests/` и `benchmarks/` |
| Runtime sanitizers | ASan/UBSan пройдены |
| Race-detection target | Helgrind завершён с `0 / 0 failed`; MT test executable в v1.0.0 отсутствует |
| Distribution | Header и `libjson_lib.a` сформированы |
| Export audit | Ровно 19 exported `json_*` symbols; legacy `jm_*` symbols отсутствуют |
| Documentation/format | Проверены Doxygen file headers, 19 public declarations/definitions и trailing whitespace |
| Benchmark protocol | Подтверждён при direct run и Makefile benchmark targets |

### Known limitations

`json_document_parse_text()` принимает только document с root object. Строковые escape-последовательности проверяются и сохраняются в raw form; `json_token_copy_text()` не выполняет Unicode decoding или normalization.

В cloud-среде benchmark binary и Makefile performance targets запускаются, но `perf record` создаёт data-файл без samples. Поэтому `perf report` в этой среде не является основанием для instruction-level performance conclusions.

### CI/CD status at tag publication

На момент публикации `v1.0.0` в repository отсутствовали workflow files в `.github/workflows/`, а GitHub Actions не содержал workflow runs или иных обнаруженных runs для commit `38a21bc`. Следовательно, удалённый CI status для данного тега **не может быть заявлен как успешно пройденный**. Future-release CI templates добавлены локально отдельным изменением после `v1.0.0`; их необходимо закоммитить и опубликовать, после чего следующий release tag будет проверяться автоматически.

[1.0.0]: https://github.com/kirill-bayborodov/json-lib/tree/v1.0.0

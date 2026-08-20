# Release process и semantic versioning

Этот документ является операционным шаблоном будущих поставок `json-lib`. Он определяет единую последовательность: классификация версии, изменение кода и документации, локальный quality gate, подготовка changelog, CI-проверка и только затем создание аннотированного Git-тега и GitHub Release.

> **Непреложное правило.** Тег версии создаётся только для чистого commit, уже прошедшего локальные quality gates. Тег не перемещается и не перезаписывается. Если после публикации обнаружена ошибка, выпускается следующая версия, а не изменяется существующий release tag.

## 1. Модель версии

Используется [Semantic Versioning 2.0.0](https://semver.org/lang/ru/):

```text
MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
```

| Часть | Когда увеличивать | Примеры для json-lib |
|---|---|---|
| `MAJOR` | Несовместимое изменение public API или поведения | Удаление/переименование `json_*` API, изменение значения status, изменение root-document contract, несовместимая ownership semantics |
| `MINOR` | Новая обратно совместимая возможность | Новый status-only public API, новый typed conversion, новая опциональная writer capability, дополнительный backward-compatible benchmark mode |
| `PATCH` | Исправление без изменения public contract | Parser bug fix, memory-safety correction, documentation clarification, test improvement, Makefile fix без изменения API |
| `-alpha.N` | Ранняя экспериментальная поставка | `v1.1.0-alpha.1` |
| `-beta.N` | Feature-complete кандидат для внешней проверки | `v1.1.0-beta.1` |
| `-rc.N` | Release candidate после полного QG | `v1.1.0-rc.1` |
| `+BUILD` | Метаданные сборки, не влияющие на precedence | `v1.1.0+sha.abcdef0` |

Предрелизные теги не публикуются как production release до явного review. Для регулярного public release используется аннотированный тег вида `vMAJOR.MINOR.PATCH`, например `v1.0.1`.

## 2. Conventional commit mapping

Commit subject использует форму `type(scope): summary`. Это позволяет review и будущей автоматизации предсказуемо определять потенциальный version bump.

| Commit form | Предварительная классификация | Требуемая проверка человеком |
|---|---|---|
| `fix:` | PATCH | Подтвердить отсутствие изменения public API и output semantics |
| `docs:`, `test:`, `build:`, `ci:` | Обычно без release или PATCH | Подтвердить, что изменение действительно поставляется пользователям |
| `feat:` | MINOR | Подтвердить backward compatibility и обновить public API/README/tests |
| `feat!:` или `BREAKING CHANGE:` в body | MAJOR | Подтвердить migration guide, обновить changelog и release notes |
| `perf:` | PATCH либо MINOR | Подтвердить reproducible benchmark evidence и отсутствие semantic regression |
| `refactor:` | Обычно без release или PATCH | Подтвердить, что public semantics и ABI не изменились |

Автоматическое определение версии — только предложение. Финальное решение принимает maintainer после review public header, status codes, ownership и compatibility impact.

## 3. Подготовка изменения

Каждое изменение API, parser-а, writer-а или Makefile должно включать согласованные source, test и documentation updates.

| Изменение | Обязательные обновления |
|---|---|
| Новый или изменённый `json_*` API | `include/json_lib.h`, `src/json_lib.c`, deterministic tests, README, changelog category |
| Новый status code или изменение status semantics | Header enum, inline comments, implementation, all affected tests, README status table, migration note при несовместимости |
| Parser grammar или token model | Parser tests, stress test при lifecycle impact, README grammar section, benchmark review если изменился hot path |
| Writer transaction | Integration test с real artifact, error-path coverage, README writer how-to |
| Benchmark protocol | Benchmark source, Makefile marker checks, README protocol example |
| CI/release workflow | Workflow YAML, этот документ, dry-run review до release tag |

## 4. Локальный quality gate

Перед созданием release commit и tag выполните полный минимальный QG в чистом working tree:

```bash
cd ~/projects/json-lib

make clean
make build SAN=no
make test SAN=no
make lint
make test_sanitize
make test_helgrind
make dist

nm -g --defined-only dist/lib/libjson_lib.a \
  | awk '{print $3}' \
  | grep '^json_' \
  | sort

git diff --check
git status --short
```

| Проверка | Release acceptance criterion |
|---|---|
| Strict C11 build | Нет compiler warning/error при `-Werror -pedantic` |
| `make test` | `=== Summary: 0 / N failed ===` |
| `make lint` | cppcheck завершается с кодом 0 |
| `make test_sanitize` | ASan/UBSan без runtime error |
| `make test_helgrind` | Нет Helgrind failures для каждого существующего `_mt` executable |
| `make dist` | Существуют `dist/include/json_lib.h` и `dist/lib/libjson_lib.a` |
| Export audit | Только ожидаемые public `json_*` symbols |
| Documentation audit | Нет trailing whitespace; Doxygen и README согласованы с API |
| Benchmark | Protocol line предшествует `Benchmark finished.`; perf limitations задокументированы отдельно от результата binary |

Нельзя выпускать тег, если любой обязательный QG не пройден. Ограничение самой среды — например, отсутствие `perf` samples — фиксируется в release notes как limitation, но не маскируется как successful profiling evidence.

## 5. Release checklist

Скопируйте этот checklist в PR/release issue и отметьте каждый пункт фактами, а не предположениями.

| Шаг | Ответственный результат |
|---|---|
| Классифицирован SemVer bump | Выбраны current и next versions, compatibility reviewed |
| Public contract reviewed | Header, statuses, ownership, API count и migration impact проверены |
| QG завершён | Логи/CI URL либо точные команды и итоговые summaries сохранены |
| `CHANGELOG.md` обновлён | Entry описывает только изменения с предыдущего тега |
| Release notes подготовлены | Включают compatibility impact, QG и known limitations |
| Commit создан и reviewed | Commit SHA известен, working tree clean |
| CI на release commit завершён | Workflow conclusion `success` для всех required jobs |
| Аннотированный тег создан | Тег вида `vX.Y.Z` указывает на reviewed commit |
| Tag CI завершён | Workflow event/tag/ref и all required checks `success` |
| GitHub Release опубликован | Notes и distribution asset относятся к тому же tag |

## 6. Команды публикации

### 6.1. Проверка следующей версии

```bash
CURRENT_TAG=$(git describe --tags --abbrev=0)
NEXT_TAG=v1.0.1

git log --format='%h %s' "${CURRENT_TAG}..HEAD"
git diff --check "${CURRENT_TAG}..HEAD"
git status --short
```

### 6.2. Создание release commit

```bash
git add CHANGELOG.md README.md include src tests benchmarks Makefile .github docs
git diff --cached --check
git commit -m "chore(release): prepare ${NEXT_TAG}"
```

При команде `git add` включайте только существующие и намеренно изменённые пути. Не добавляйте generated `build/`, `bin/`, `dist/` или benchmark reports.

### 6.3. Публикация commit и ожидание main CI

```bash
git push origin main

gh run list \
  --repo kirill-bayborodov/json-lib \
  --branch main \
  --limit 10
```

Публикация main и создание tag являются разными действиями. Создавайте tag только после того, как remote CI для release commit завершён с `success`.

### 6.4. Создание и публикация аннотированного тега

```bash
git tag -a "${NEXT_TAG}" -m "json-lib ${NEXT_TAG}" \
  -m "<краткое описание совместимого изменения>" \
  -m "QG: <фактически пройденные проверки и ограничения среды>."

git push origin "${NEXT_TAG}"
```

После tag push дождитесь tag-triggered CI:

```bash
gh run list \
  --repo kirill-bayborodov/json-lib \
  --commit "$(git rev-list -n 1 "${NEXT_TAG}")" \
  --limit 20
```

### 6.5. Верификация refs

```bash
git fetch --tags origin
git ls-remote --heads --tags origin \
  refs/heads/main "refs/tags/${NEXT_TAG}" "refs/tags/${NEXT_TAG}^{}"
git status --short --branch
```

Аннотированный tag создаёт отдельный tag object и peeled ref `^{}`. Peeled ref должен совпадать с release commit SHA.

## 7. Шаблон release notes

Скопируйте блок ниже в GitHub Release и замените все placeholders подтверждёнными данными.

```markdown
# json-lib vX.Y.Z

**Commit:** `<full-SHA>`

**Previous version:** `vA.B.C`

**Compatibility:** `<backward-compatible | breaking; ссылка на migration guide>`

## Added

<Новые user-visible возможности.>

## Changed

<Изменённое поведение и совместимость.>

## Fixed

<Исправленные defects.>

## Quality gates

| Check | Result |
|---|---|
| Strict C11 build | `<result>` |
| Tests | `<result>` |
| Static analysis | `<result>` |
| Sanitizers | `<result>` |
| Helgrind | `<result>` |
| Distribution/export audit | `<result>` |
| CI for tag | `<workflow URL and success conclusion>` |

## Known limitations

<Только подтверждённые environment или product limitations.>
```

## 8. Запреты и аварийная процедура

Никогда не применяйте `git tag -f`, `git push --force --tags` или `git push --force origin main` для опубликованного release. Для исправления критической ошибки выпустите следующий PATCH или, при несовместимом hotfix, корректно классифицированный MINOR/MAJOR release с явным описанием причины.

Если tag создан, но CI не прошёл, не объявляйте release готовым. Создайте исправляющий commit, повторите QG, увеличьте версию согласно SemVer и выпустите новый tag. Не удаляйте опубликованный tag без специального security/operations решения, явно зафиксированного maintainer-ом.

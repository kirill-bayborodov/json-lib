---
name: Release preparation
description: Prepare and review a semantic-versioned json-lib release
title: "chore(release): prepare vX.Y.Z"
labels:
  - release
assignees: []
---

# Release preparation: `vX.Y.Z`

> Этот шаблон создаёт review record, но не заменяет проверяемые command logs и CI URLs. Не создавайте tag и не публикуйте GitHub Release до закрытия каждого обязательного пункта.

## Версия и совместимость

| Поле | Подтверждённое значение |
|---|---|
| Предыдущая версия | `vA.B.C` |
| Предлагаемая версия | `vX.Y.Z` |
| SemVer class | `MAJOR` / `MINOR` / `PATCH` |
| Release commit SHA | `<full SHA>` |
| Backward compatibility | `yes` / `no`; при `no` — ссылка на migration guide |
| Причина version bump | `<проверяемое объяснение>` |

## Изменения для пользователей

### Added

<!-- Новые публичные возможности; укажите API, ограничения и usage impact. -->

### Changed

<!-- Изменения поведения; укажите compatibility impact. -->

### Fixed

<!-- Исправленные дефекты; укажите проверяемые symptoms и test coverage. -->

### Known limitations

<!-- Только подтверждённые ограничения среды/продукта. -->

## Локальный quality gate

- [ ] `make clean && make build CONFIG=release SAN=no` завершён успешно.
- [ ] `make test CONFIG=release SAN=no` завершён с `0 / N failed`.
- [ ] `make lint` завершён успешно.
- [ ] `make test_sanitize CONFIG=debug SAN=address` завершён успешно.
- [ ] `make test_helgrind CONFIG=debug` завершён успешно.
- [ ] `make dist CONFIG=release SAN=no` сформировал header и static archive.
- [ ] Export list reviewed: только ожидаемые `json_*` symbols.
- [ ] README, Doxygen и `CHANGELOG.md` согласованы с поставляемым API.
- [ ] Benchmark protocol проверен; perf limitations описаны отдельно от functional result.

## Удалённый CI/CD

| Проверка | URL или результат |
|---|---|
| `main` quality-gate workflow для release commit | `<URL>` |
| Conclusion всех required jobs | `success` |
| Tag workflow для `vX.Y.Z` | `<URL>` |
| Release artifact и checksum | `<URL>` |

## Публикация

- [ ] Release commit reviewed и опубликован в `origin/main`.
- [ ] Аннотированный tag `vX.Y.Z` создан для reviewed SHA.
- [ ] Tag опубликован без force push.
- [ ] Tag-triggered CI завершился `success`.
- [ ] GitHub Release создан из того же тега и содержит tested distribution asset.

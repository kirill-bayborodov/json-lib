# Usage Guide: интеграция `json-lib` в сторонний C11-проект

Этот документ описывает воспроизводимую интеграцию `json-lib` в независимый проект на ISO C11. Библиотека поставляется как public header `json_lib.h` и static archive `libjson_lib.a`, не требует runtime JSON dependency и предоставляет только status-returning API.

> **Ключевой контракт.** Все library functions возвращают `json_status_t`; полезные значения передаются через output-параметры. Только `int main(void)` в исполняемой программе потребителя возвращает `int` как граница ISO C.

## 1. Выбор способа подключения

| Вариант | Когда выбирать | Получаемый результат |
|---|---|---|
| GitHub Release archive | Нужна фиксированная проверенная версия без исходной истории | `include/json_lib.h` и `lib/libjson_lib.a` из release asset |
| Git submodule | Нужна воспроизводимая source dependency, обновляемая отдельным commit | Исходный код, Makefile, tests и documentation внутри `third_party/json-lib` |
| Source vendor copy | Проект использует собственный vendor workflow | Копии `include/`, `src/` и лицензии под контролем потребителя |

Для production integration рекомендуется pin на аннотированный SemVer tag, а не на ветку `main`. Список проверенных архивов доступен в [GitHub Releases](https://github.com/kirill-bayborodov/json-lib/releases).

## 2. Вариант A: подключение из release archive

Скачайте archive и его checksum из нужного GitHub Release. Ниже `v1.0.2` приведён как пример; замените его на согласованную версию зависимости.

```bash
mkdir -p third_party/json-lib
cd third_party/json-lib

curl --fail --location --remote-name \
  https://github.com/kirill-bayborodov/json-lib/releases/download/v1.0.2/json-lib-v1.0.2.tar.gz
curl --fail --location --remote-name \
  https://github.com/kirill-bayborodov/json-lib/releases/download/v1.0.2/json-lib-v1.0.2.tar.gz.sha256

sha256sum -c json-lib-v1.0.2.tar.gz.sha256
mkdir -p v1.0.2
tar -xzf json-lib-v1.0.2.tar.gz -C v1.0.2
```

Успешная checksum verification обязательна до компиляции. Archive содержит только public distribution: `LICENSE`, `README.md`, `include/json_lib.h` и `lib/libjson_lib.a`.

### 2.1. Linking command

Расположите `-ljson_lib` **до** `-lm` и после object files потребителя. Это сохраняет корректный порядок static linker resolution.

```bash
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -Ithird_party/json-lib/v1.0.2/include \
  -c src/app.c -o build/app.o

cc build/app.o \
  -Lthird_party/json-lib/v1.0.2/lib -ljson_lib -lm \
  -o bin/app
```

`-lm` необходим, поскольку библиотека преобразует JSON number в `double`. При использовании `json_writer_t` на POSIX-платформе не требуется дополнительная библиотека: writer использует стандартные file operations и `fsync` из системной C/POSIX среды.

### 2.2. Минимальный Makefile fragment

```make
JSON_LIB_ROOT := third_party/json-lib/v1.0.2
JSON_LIB_CFLAGS := -I$(JSON_LIB_ROOT)/include
JSON_LIB_LDFLAGS := -L$(JSON_LIB_ROOT)/lib -ljson_lib -lm

build/app.o: src/app.c
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic \
		$(JSON_LIB_CFLAGS) -c $< -o $@

bin/app: build/app.o
	$(CC) $^ $(JSON_LIB_LDFLAGS) -o $@
```

Не добавляйте generated directories `build/`, `bin/` и `dist/` самой библиотеки в свой Git index. Для binary reproducibility фиксируйте version, release checksum и compiler flags в собственном dependency manifest.

## 3. Вариант B: Git submodule

Submodule удобен, когда проекту нужны исходники, локальный QG или controlled update dependency.

```bash
git submodule add \
  https://github.com/kirill-bayborodov/json-lib.git \
  third_party/json-lib

git -C third_party/json-lib checkout v1.0.2
make -C third_party/json-lib clean
make -C third_party/json-lib dist CONFIG=release SAN=no
```

После этого link against `third_party/json-lib/dist/include` и `third_party/json-lib/dist/lib`:

```bash
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -Ithird_party/json-lib/dist/include \
  src/app.c third_party/json-lib/dist/lib/libjson_lib.a -lm \
  -o bin/app
```

Закоммитьте `.gitmodules` и gitlink submodule. Обновление зависимости выполняйте отдельным reviewable commit и повторяйте QG вашего проекта.

## 4. Parser usage: object lookup и type conversion

`json_document_parse_text()` принимает только JSON document с root object. Инициализируйте document перед parse и всегда вызывайте `json_document_destroy()` после завершения работы, включая error paths.

```c
#include <stdio.h>
#include <stdlib.h>

#include "json_lib.h"

int main(void)
{
    static const char text[] =
        "{\"service\":\"worker\",\"workers\":4,\"enabled\":true}";
    json_document_t document;
    json_status_t status;
    size_t workers_index;
    size_t enabled_index;
    uint64_t workers;
    json_boolean_t enabled;
    char error[256];
    int exit_code = EXIT_FAILURE;

    status = json_document_init(&document);
    if (status != JSON_STATUS_SUCCESS) {
        return EXIT_FAILURE;
    }

    status = json_document_parse_text(text, &document, error, sizeof(error));
    if (status != JSON_STATUS_SUCCESS) {
        (void)fprintf(stderr, "JSON parse failed: %s\n", error);
        (void)json_document_destroy(&document);
        return EXIT_FAILURE;
    }

    status = json_object_get(&document, 0U, "workers", &workers_index);
    if (status == JSON_STATUS_SUCCESS) {
        status = json_token_to_u64(&document, workers_index, &workers);
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_object_get(&document, 0U, "enabled", &enabled_index);
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_token_to_boolean(&document, enabled_index, &enabled);
    }
    if (status == JSON_STATUS_SUCCESS) {
        (void)printf("workers=%llu enabled=%s\n",
            (unsigned long long)workers,
            enabled == JSON_BOOLEAN_TRUE ? "true" : "false");
        exit_code = EXIT_SUCCESS;
    } else {
        (void)fprintf(stderr, "JSON field conversion failed: status=%d\n", (int)status);
    }

    (void)json_document_destroy(&document);
    return exit_code;
}
```

`object_index == 0U` означает root token. `json_object_get()` обходит только непосредственные key/value pairs и возвращает `JSON_STATUS_NOT_FOUND`, если ключ отсутствует. Несовместимое преобразование — например, попытка преобразовать string в `uint64_t` — возвращает `JSON_STATUS_SYNTAX_ERROR`.

## 5. Навигация по array и raw string ownership

Ниже документ содержит array и показывает transfer ownership строки из `json_token_copy_text()`.

```c
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "json_lib.h"

int main(void)
{
    static const char text[] =
        "{\"labels\":[\"fast\",\"stable\"]}";
    json_document_t document;
    json_status_t status;
    size_t labels_index;
    size_t label_count;
    size_t first_label_index = 0U;
    char *label = NULL;
    char error[256];
    int exit_code = EXIT_FAILURE;

    status = json_document_init(&document);
    if (status != JSON_STATUS_SUCCESS) {
        return EXIT_FAILURE;
    }

    status = json_document_parse_text(text, &document, error, sizeof(error));
    if (status == JSON_STATUS_SUCCESS) {
        status = json_object_get(&document, 0U, "labels", &labels_index);
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_array_size(&document, labels_index, &label_count);
    }
    if (status == JSON_STATUS_SUCCESS && label_count == 0U) {
        status = JSON_STATUS_NOT_FOUND;
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_array_get(&document, labels_index, 0U, &first_label_index);
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_token_copy_text(&document, first_label_index, &label);
    }
    if (status == JSON_STATUS_SUCCESS) {
        (void)printf("first label: %s\n", label);
        exit_code = EXIT_SUCCESS;
    } else {
        (void)fprintf(stderr, "JSON navigation failed: status=%d\n", (int)status);
    }

    (void)json_memory_free(label);
    (void)json_document_destroy(&document);
    return exit_code;
}
```

`json_token_copy_text()` возвращает owned NUL-terminated allocation. Освобождайте её только через `json_memory_free()`, даже если `json_document_destroy()` уже завершился. Для string token returned text не содержит внешних JSON quotes, но сохраняет raw escape sequences; Unicode decoding и normalization не выполняются. Пустой array в этом примере явно преобразуется в `JSON_STATUS_NOT_FOUND`, поэтому `first_label_index` не читается до успешного `json_array_get()`.

## 6. File loading и diagnostics

`json_document_load_file()` различает filesystem failure (`JSON_STATUS_IO_ERROR`) и JSON grammar/type failure (`JSON_STATUS_SYNTAX_ERROR`). Передавайте отдельный diagnostic buffer, чтобы сохранить parse explanation.

```c
json_document_t document;
json_status_t status;
char error[256];

status = json_document_init(&document);
if (status == JSON_STATUS_SUCCESS) {
    status = json_document_load_file("config.json", &document, error, sizeof(error));
}
if (status != JSON_STATUS_SUCCESS) {
    (void)fprintf(stderr, "config.json: status=%d diagnostic=%s\n",
        (int)status, error);
}
(void)json_document_destroy(&document);
```

`error` is optional: передайте `NULL` и `0U`, если application не выводит human-readable diagnostic. Output parameters необходимо читать только при `JSON_STATUS_SUCCESS`.

## 7. Atomic writer usage

Writer публикует target через temporary file в том же каталоге. `json_writer_write_raw()` предназначен только для trusted JSON punctuation/literals; untrusted strings передавайте в `json_writer_write_string()`.

```c
#include <stdio.h>
#include <stdlib.h>

#include "json_lib.h"

int main(void)
{
    json_writer_t writer = {0};
    json_status_t status;
    char error[256] = {0};
    int exit_code = EXIT_FAILURE;

    status = json_writer_open("config.json", &writer, error, sizeof(error));
    if (status == JSON_STATUS_SUCCESS) {
        status = json_writer_write_raw(&writer, "{\"name\":");
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_writer_write_string(&writer, "worker \"A\"");
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_writer_write_raw(&writer, ",\"enabled\":true}\n");
    }
    if (status == JSON_STATUS_SUCCESS) {
        status = json_writer_commit(&writer, error, sizeof(error));
    }
    if (status == JSON_STATUS_SUCCESS) {
        exit_code = EXIT_SUCCESS;
    } else {
        (void)fprintf(stderr, "write failed: status=%d diagnostic=%s\n",
            (int)status, error);
        (void)json_writer_abort(&writer);
    }

    return exit_code;
}
```

`json_writer_commit()` performs `fflush`, `fsync`, `fclose` and `rename`. Atomic replacement therefore applies when temporary path and target path are located on the same filesystem; `json_writer_open()` intentionally creates the temporary file beside the target. Call `json_writer_abort()` after every failed transaction path, including failure after a partial write.

## 8. Status handling quick reference

| Status | Typical consumer response |
|---|---|
| `JSON_STATUS_SUCCESS` | Read documented output parameters and continue. |
| `JSON_STATUS_ARGUMENT_ERROR` | Correct caller logic: NULL, index, token type or object lifecycle is invalid. |
| `JSON_STATUS_NOT_FOUND` | Apply application default or report missing optional field. |
| `JSON_STATUS_SYNTAX_ERROR` | Reject malformed JSON or an incompatible requested conversion. |
| `JSON_STATUS_ALLOCATION_ERROR` | Report resource exhaustion; preserve any valid prior configuration independently. |
| `JSON_STATUS_CAPACITY_ERROR` | Reject overly large document safely; consider an application-level input limit. |
| `JSON_STATUS_IO_ERROR` | Retry or surface filesystem/stream error; inspect diagnostic buffer where supplied. |

Do not inspect output parameters after a non-success status unless a specific API contract explicitly states otherwise.

## 9. Consumer-side quality gate

Добавьте integration smoke test в CI потребителя. Он должен compile example source with the exact include/library paths, execute parse/navigation/writer behavior in a temporary directory и verify that `json_memory_free()`/`json_document_destroy()` are called on all owned resources.

```bash
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -Ithird_party/json-lib/v1.0.2/include \
  tests/json_lib_integration.c \
  third_party/json-lib/v1.0.2/lib/libjson_lib.a -lm \
  -o build/json_lib_integration

./build/json_lib_integration
```

Проверяйте обновление зависимости отдельно: скачайте новый release archive, проверьте её SHA-256, review `CHANGELOG.md` и `docs/RELEASE_PROCESS.md`, затем повторите consumer tests before committing the new pinned version.

## 10. Генерируемая API documentation

Documentation workflow генерирует Doxygen из `Doxyfile` и публикует только static HTML artifact через GitHub Pages после успешного repository-level deployment. До включения или проверки Pages локальную HTML documentation можно построить так:

```bash
sudo apt-get update
sudo apt-get install --yes doxygen

doxygen Doxyfile
xdg-open docs/generated/html/index.html
```

Generated path `docs/generated/` не является source artifact и не должен коммититься. Для explanation release pipeline и SemVer смотрите [Release process](RELEASE_PROCESS.md).

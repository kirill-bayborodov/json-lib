/**
 * @file json_lib.h
 * @brief Публичный C11-интерфейс самостоятельной библиотеки разбора и записи JSON.
 * @details
 * `json-lib` принимает UTF-8-совместимый JSON-текст, синтаксически проверяет его
 * и строит плоский массив токенов в префиксном порядке. Каждый токен хранит
 * диапазон исходного текста и индекс родителя; поэтому библиотека не формирует
 * рекурсивное дерево и позволяет безопасно выполнять навигацию только через
 * проверяемые индексы. Исходный текст и массив токенов принадлежат
 * `json_document_t` до вызова `json_document_destroy()`.
 *
 * Все публичные операции возвращают только `json_status_t`. Полезные значения,
 * включая предикаты, числовые преобразования и индексы токенов, передаются через
 * output-параметры. Единственная функция, которой разрешено возвращать `int`,
 * находится в исполняемых программах-потребителях и является ISO C точкой входа
 * `main`; она не входит в API этой библиотеки.
 *
 * @par Потоковая безопасность
 * Библиотека не содержит глобального изменяемого состояния. Независимые объекты
 * `json_document_t` и `json_writer_t` допускается использовать одновременно в
 * разных потоках. Один объект не должен использоваться конкурентно без внешней
 * синхронизации, потому что parse/destroy и writer-операции изменяют его поля.
 */
#ifndef JSON_LIB_H
#define JSON_LIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * @brief Представляет именованный итог выполнения операции json-lib.
 * @details
 * При `JSON_STATUS_SUCCESS` библиотека гарантирует инициализацию всех output-
 * параметров, описанных контрактом функции. При любом другом значении вызывающий
 * код не должен читать output-параметры, если функция не документирует обратное.
 */
typedef enum {
    JSON_STATUS_SUCCESS = 0,          /**< Операция завершена успешно; outputs записаны. */
    JSON_STATUS_ARGUMENT_ERROR = 1,   /**< Передан NULL, неинициализированный объект, неверный индекс или параметр. */
    JSON_STATUS_NOT_FOUND = 2,        /**< Запрошенный ключ объекта либо позиция элемента массива отсутствует. */
    JSON_STATUS_SYNTAX_ERROR = 3,     /**< Вход не удовлетворяет JSON-грамматике или ожидаемому primitive type. */
    JSON_STATUS_ALLOCATION_ERROR = 4, /**< Не удалось выделить память для текста, токенов или пути writer-а. */
    JSON_STATUS_CAPACITY_ERROR = 5,   /**< Требуемый размер массива токенов превышает безопасный предел `size_t`. */
    JSON_STATUS_IO_ERROR = 6          /**< Операция над файлом, stream, fsync, close либо atomic rename завершилась ошибкой. */
} json_status_t;

/**
 * @brief Представляет типизированный результат JSON-предиката.
 * @details
 * Тип исключает использование неименованных целочисленных литералов в API
 * `json_token_has_type()` и `json_token_equals()`.
 */
typedef enum {
    JSON_BOOLEAN_FALSE = 0, /**< Проверяемое условие не выполняется. */
    JSON_BOOLEAN_TRUE = 1   /**< Проверяемое условие выполняется. */
} json_boolean_t;

/**
 * @brief Описывает синтаксическую категорию сохранённого JSON-токена.
 * @details
 * Строковый токен хранит байты между кавычками в исходном JSON, то есть без
 * внешних `"`, но с исходными escape-последовательностями. Primitive-токен
 * представляет number, `true`, `false` или `null`.
 */
typedef enum {
    JSON_TOKEN_UNDEFINED = 0, /**< Внутренний незанятый слот; в успешном документе не выдаётся. */
    JSON_TOKEN_OBJECT = 1,    /**< Объект, открывающийся `{` и закрывающийся `}`. */
    JSON_TOKEN_ARRAY = 2,     /**< Массив, открывающийся `[` и закрывающийся `]`. */
    JSON_TOKEN_STRING = 3,    /**< Строка без внешних кавычек. */
    JSON_TOKEN_PRIMITIVE = 4  /**< JSON number, boolean literal или null literal. */
} json_token_type_t;

/**
 * @brief Хранит положение одного лексического JSON-токена и его место в иерархии.
 * @details
 * Диапазон `[start, end)` индексирует байты поля `json_document_t::text`.
 * `parent_index` образует плоское дерево: у root-токена он равен `-1`, у каждого
 * другого токена — индексу контейнера. Поле `child_count` относится только к
 * непосредственным потомкам array/object; у объекта учитываются как key strings,
 * так и соответствующие value tokens.
 */
typedef struct {
    json_token_type_t type;  /**< Синтаксическая категория токена. */
    size_t start;            /**< Включаемое смещение начала в `document->text`. */
    size_t end;              /**< Исключаемое смещение конца в `document->text`. */
    size_t child_count;      /**< Число непосредственных потомков контейнера. */
    ptrdiff_t parent_index;  /**< Индекс токена-родителя либо `-1` для корня. */
} json_token_t;

/**
 * @brief Владеет копией JSON-текста и результатом его разбора.
 * @details
 * Перед первым использованием объект должен быть инициализирован через
 * `json_document_init()`. Операции `json_document_parse_text()` и
 * `json_document_load_file()` заменяют ранее сохранённый документ только после
 * предварительного освобождения его ресурсов. Публичные поля предназначены для
 * read-only диагностики; пользователь не должен изменять их либо освобождать
 * принадлежащую объекту память напрямую.
 */
typedef struct {
    char *text;              /**< Owned NUL-terminated копия исходного JSON-текста. */
    json_token_t *tokens;    /**< Owned префиксный массив токенов, ссылающихся на `text`. */
    size_t text_size;        /**< Число байтов JSON до завершающего NUL. */
    size_t token_count;      /**< Число инициализированных элементов `tokens`. */
} json_document_t;

/**
 * @brief Владеет состоянием незавершённой atomic JSON file transaction.
 * @details
 * `json_writer_open()` создаёт временный файл в каталоге целевого файла.
 * `json_writer_write_raw()` и `json_writer_write_string()` записывают в этот
 * файл. `json_writer_commit()` выполняет flush, fsync и rename, после чего
 * временный путь атомарно становится целевым на одной файловой системе.
 * `json_writer_abort()` удаляет временный файл и возвращает объект в пустое
 * состояние; его безопасно вызывать после частично неудачного открытия.
 */
typedef struct {
    FILE *stream;             /**< Owned writable temporary stream до commit/abort. */
    char *temporary_path;     /**< Owned NUL-terminated путь временного файла. */
    char *target_path;        /**< Owned NUL-terminated путь публикуемого файла. */
} json_writer_t;

/**
 * @brief Инициализирует document как пустой объект без выделения памяти.
 * @param document Output-объект, который требуется инициализировать.
 * @return `JSON_STATUS_SUCCESS` либо `JSON_STATUS_ARGUMENT_ERROR`.
 * @details
 * Алгоритм записывает нулевой aggregate. После успеха документ можно передать
 * любой parse/load операции или безопасно освободить через
 * `json_document_destroy()`.
 */
json_status_t json_document_init(json_document_t *document);

/**
 * @brief Разбирает NUL-terminated JSON-текст, у которого корнем является object.
 * @param text Входной JSON-текст, не принадлежащий библиотеке.
 * @param document Инициализированный output-документ для результата разбора.
 * @param error Необязательный буфер человекочитаемой диагностики.
 * @param error_capacity Ёмкость `error` в байтах; ноль разрешён при `error == NULL`.
 * @return Именованный `json_status_t` результата разбора.
 * @details
 * Алгоритм копирует исходный текст, выполняет синтаксический recursive-descent
 * разбор в плоский префиксный token array и при заполнении capacity повторяет
 * разбор с удвоенной ёмкостью. Успешным считается только документ с root object;
 * при ошибке частично выделенные ресурсы освобождаются, а `document` возвращается
 * в пустое состояние.
 */
json_status_t json_document_parse_text(const char *text, json_document_t *document,
    char *error, size_t error_capacity);

/**
 * @brief Загружает JSON-файл и разбирает его как document с root object.
 * @param path Путь к читаемому regular file.
 * @param document Инициализированный output-документ.
 * @param error Необязательный буфер диагностики синтаксической ошибки.
 * @param error_capacity Ёмкость `error` в байтах.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм открывает файл в бинарном режиме, определяет длину, считывает ровно
 * это число байтов в NUL-terminated буфер и передаёт буфер
 * `json_document_parse_text()`. Ошибки доступа и чтения отличаются от ошибок
 * JSON-грамматики кодом `JSON_STATUS_IO_ERROR`.
 */
json_status_t json_document_load_file(const char *path, json_document_t *document,
    char *error, size_t error_capacity);

/**
 * @brief Освобождает все ресурсы document и повторно инициализирует его.
 * @param document Инициализированный document для освобождения.
 * @return `JSON_STATUS_SUCCESS` либо `JSON_STATUS_ARGUMENT_ERROR`.
 * @details
 * Алгоритм освобождает text и tokens независимо, после чего обнуляет aggregate.
 * Поэтому повторный вызов после успешного destroy не вызывает double free.
 */
json_status_t json_document_destroy(json_document_t *document);

/**
 * @brief Находит value token непосредственного поля JSON object по имени ключа.
 * @param document Успешно разобранный document.
 * @param object_index Индекс токена типа `JSON_TOKEN_OBJECT`.
 * @param key Искомое имя ключа без JSON-кавычек.
 * @param value_index Output-индекс токена значения.
 * @return `JSON_STATUS_SUCCESS`, `JSON_STATUS_NOT_FOUND` или error status.
 * @details
 * Алгоритм обходит непосредственные key/value пары объекта. Для каждого key
 * сравниваются raw bytes, а функция определения span пропускает всё поддерево
 * соответствующего value. Вложенные поля не участвуют в поиске.
 */
json_status_t json_object_get(const json_document_t *document, size_t object_index,
    const char *key, size_t *value_index);

/**
 * @brief Возвращает число непосредственных элементов JSON array.
 * @param document Успешно разобранный document.
 * @param array_index Индекс токена типа `JSON_TOKEN_ARRAY`.
 * @param element_count Output-число непосредственных элементов.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм проверяет тип токена и копирует сохранённое при разборе количество
 * прямых потомков массива без обхода вложенных поддеревьев.
 */
json_status_t json_array_size(const json_document_t *document, size_t array_index,
    size_t *element_count);

/**
 * @brief Возвращает индекс элемента JSON array по нулевой позиции.
 * @param document Успешно разобранный document.
 * @param array_index Индекс токена типа `JSON_TOKEN_ARRAY`.
 * @param element_position Нулевая позиция среди непосредственных элементов.
 * @param value_index Output-индекс найденного токена.
 * @return `JSON_STATUS_SUCCESS`, `JSON_STATUS_NOT_FOUND` или error status.
 * @details
 * Алгоритм последовательно обходит прямых потомков массива, пропуская целые
 * вложенные subtrees. Благодаря этому позиция не зависит от числа токенов в
 * предыдущем объекте или массиве.
 */
json_status_t json_array_get(const json_document_t *document, size_t array_index,
    size_t element_position, size_t *value_index);

/**
 * @brief Сравнивает синтаксический тип токена с ожидаемым типом.
 * @param document Успешно разобранный document.
 * @param token_index Индекс проверяемого токена.
 * @param expected_type Ожидаемая категория токена.
 * @param result Output-предикат равенства типов.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм валидирует документ и индекс, затем записывает `JSON_BOOLEAN_TRUE`
 * либо `JSON_BOOLEAN_FALSE`; несовпадение типа не является ошибкой операции.
 */
json_status_t json_token_has_type(const json_document_t *document, size_t token_index,
    json_token_type_t expected_type, json_boolean_t *result);

/**
 * @brief Проверяет raw text токена на точное побайтное равенство строке.
 * @param document Успешно разобранный document.
 * @param token_index Индекс сравниваемого токена.
 * @param expected_text Ожидаемый текст без JSON-кавычек для string token.
 * @param result Output-предикат равенства.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм сначала сравнивает длины, затем выполняет `memcmp()` по диапазону
 * `[start, end)`. Escape-последовательности string token не декодируются:
 * сравнение выполняется с raw JSON representation.
 */
json_status_t json_token_equals(const json_document_t *document, size_t token_index,
    const char *expected_text, json_boolean_t *result);

/**
 * @brief Копирует raw text токена в отдельную NUL-terminated строку.
 * @param document Успешно разобранный document.
 * @param token_index Индекс копируемого токена.
 * @param text Output-указатель на allocation, освобождаемую json_memory_free().
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм проверяет индекс, выделяет `token_length + 1` байт, копирует raw
 * диапазон токена и добавляет завершающий NUL. Для строк кавычки не копируются,
 * но JSON escape-последовательности остаются неизменными.
 */
json_status_t json_token_copy_text(const json_document_t *document, size_t token_index,
    char **text);

/**
 * @brief Преобразует JSON number primitive в `uint64_t` через output-параметр.
 * @param document Успешно разобранный document.
 * @param token_index Индекс токена типа `JSON_TOKEN_PRIMITIVE`.
 * @param value Output-беззнаковое 64-битное значение.
 * @return `JSON_STATUS_SUCCESS`, `JSON_STATUS_SYNTAX_ERROR` или error status.
 * @details
 * Алгоритм копирует raw primitive, применяет `strtoull()` с основанием десять и
 * принимает результат только при полном потреблении строки и отсутствии overflow.
 * Отрицательные, дробные и экспоненциальные JSON numbers отклоняются как
 * неподходящие для `uint64_t`.
 */
json_status_t json_token_to_u64(const json_document_t *document, size_t token_index,
    uint64_t *value);

/**
 * @brief Преобразует JSON number primitive в конечное значение `double`.
 * @param document Успешно разобранный document.
 * @param token_index Индекс токена типа `JSON_TOKEN_PRIMITIVE`.
 * @param value Output-конечное floating-point значение.
 * @return `JSON_STATUS_SUCCESS`, `JSON_STATUS_SYNTAX_ERROR` или error status.
 * @details
 * Алгоритм копирует raw primitive, применяет `strtod()` и требует полного
 * потребления текста, отсутствия range error и конечного результата. JSON
 * literals `true`, `false` и `null` не удовлетворяют этому контракту.
 */
json_status_t json_token_to_double(const json_document_t *document, size_t token_index,
    double *value);

/**
 * @brief Преобразует primitive literal `true` или `false` в json_boolean_t.
 * @param document Успешно разобранный document.
 * @param token_index Индекс токена типа `JSON_TOKEN_PRIMITIVE`.
 * @param value Output-значение `JSON_BOOLEAN_TRUE` или `JSON_BOOLEAN_FALSE`.
 * @return `JSON_STATUS_SUCCESS`, `JSON_STATUS_SYNTAX_ERROR` или error status.
 * @details
 * Алгоритм выполняет точные raw-text сравнения сначала с `true`, затем с `false`.
 * Number и `null` отклоняются без неявного приведения.
 */
json_status_t json_token_to_boolean(const json_document_t *document, size_t token_index,
    json_boolean_t *value);

/**
 * @brief Освобождает allocation, ранее возвращённый json-lib.
 * @param memory Nullable allocation, полученный через json_token_copy_text().
 * @return `JSON_STATUS_SUCCESS`.
 * @details
 * Алгоритм передаёт указатель `free()`. Нулевой указатель разрешён правилами C и
 * сохраняет единый status-returning ownership endpoint вместо void-return API.
 */
json_status_t json_memory_free(void *memory);

/**
 * @brief Открывает temporary stream для атомарной публикации JSON-файла.
 * @param target_path Конечный путь публикуемого файла.
 * @param writer Output-writer, который должен быть пустым или инициализированным.
 * @param error Необязательный буфер диагностики I/O ошибки.
 * @param error_capacity Ёмкость `error` в байтах.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм копирует конечный путь, формирует шаблон `<target>.tmp.XXXXXX`,
 * создаёт unique temporary file через `mkstemp()` и оборачивает его `FILE` stream.
 * Commit безопасен только если temporary file и target находятся в одном каталоге.
 */
json_status_t json_writer_open(const char *target_path, json_writer_t *writer,
    char *error, size_t error_capacity);

/**
 * @brief Записывает trusted raw JSON syntax fragment в temporary stream.
 * @param writer Открытый writer transaction.
 * @param text NUL-terminated фрагмент JSON syntax или предварительно проверенный literal.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм передаёт фрагмент `fputs()` без дополнительной валидации. Потребитель
 * отвечает за корректность punctuation и literals; для пользовательской строки
 * необходимо использовать json_writer_write_string().
 */
json_status_t json_writer_write_raw(json_writer_t *writer, const char *text);

/**
 * @brief Кодирует и записывает одну JSON string value с внешними кавычками.
 * @param writer Открытый writer transaction.
 * @param text Nullable NUL-terminated исходная строка; NULL кодируется как `""`.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм обрамляет значение кавычками, экранирует `"` и `\\`, а управляющие
 * байты U+0000..U+001F кодирует в форме `\\u00XX`. Неуправляющие байты копируются
 * без изменения; библиотека не выполняет Unicode normalization.
 */
json_status_t json_writer_write_string(json_writer_t *writer, const char *text);

/**
 * @brief Синхронизирует stream и атомарно публикует temporary JSON artifact.
 * @param writer Открытый writer transaction; после вызова будет пустым.
 * @param error Необязательный буфер диагностики I/O ошибки.
 * @param error_capacity Ёмкость `error` в байтах.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм вызывает `fflush()`, `fsync()`, `fclose()` и затем `rename()`. Любая
 * ошибка приводит к освобождению transaction и удалению временного файла; при
 * успехе target-path заменяется единой filesystem операцией rename.
 */
json_status_t json_writer_commit(json_writer_t *writer, char *error,
    size_t error_capacity);

/**
 * @brief Прерывает transaction, закрывает stream и удаляет temporary file.
 * @param writer Открытый или частично инициализированный writer transaction.
 * @return `JSON_STATUS_SUCCESS`, `JSON_STATUS_IO_ERROR` либо
 * `JSON_STATUS_ARGUMENT_ERROR`.
 * @details
 * Алгоритм best-effort закрывает stream и unlink-ает temporary path, освобождает
 * пути и записывает нулевой aggregate. Ошибка close/unlink отражается как
 * `JSON_STATUS_IO_ERROR`, но не препятствует очистке ownership. Его следует
 * вызывать при любом ошибочном пути после успешного json_writer_open().
 */
json_status_t json_writer_abort(json_writer_t *writer);

#endif /* JSON_LIB_H */

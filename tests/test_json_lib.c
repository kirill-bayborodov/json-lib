/**
 * @file test_json_lib.c
 * @brief Детерминированные public API и integration-проверки json-lib.
 * @details
 * Файл проверяет положительный путь parser/navigation/conversion, строгость JSON
 * grammar, distinction status codes, рост token capacity, file loading и atomic
 * writer transaction. Каждый сценарий создаёт независимые объекты и завершает
 * ownership lifecycle явным `json_document_destroy()` либо
 * `json_writer_abort()/json_writer_commit()`.
 *
 * @par QG-13.f coverage
 * Покрываются root object contract, вложенные array/object subtrees, `uint64_t`,
 * `double`, boolean и raw string conversions, отсутствующий key/array element,
 * malformed numbers/objects, invalid API arguments, parser capacity retry и
 * reader/writer integration через реальный temporary artifact.
 */
#include "json_lib.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief Представляет именованный результат одного test scenario.
 * @details
 * Test helpers используют этот тип, а единственная ISO C boundary `main` явно
 * преобразует итоговый named status в допустимый `int` exit code.
 */
typedef enum {
    TEST_STATUS_SUCCESS = 0, /**< Сценарий выполнил все ожидаемые проверки. */
    TEST_STATUS_FAILURE = 1  /**< Хотя бы один контракт API не совпал с ожиданием. */
} test_status_t;

/**
 * @brief Проверяет public parser, navigation и primitive conversion contract.
 * @return Именованный `test_status_t`.
 * @details
 * Сценарий разбирает object со scalar fields, escaped string, array и вложенным
 * object. Затем он проходит по key/value и array APIs, проверяет типы и raw-text
 * copy. В конце отдельная проверка подтверждает `JSON_STATUS_NOT_FOUND` для
 * отсутствующей array позиции и уничтожает document.
 */
static test_status_t test_parse_navigation_and_conversion(void)
{
    static const char source[] =
        "{\"n\":42,\"f\":1.5,\"ok\":true,\"escaped\":\"a\\n\","
        "\"items\":[\"x\",{\"inner\":2},null]}";
    json_document_t document;
    char error[128];
    char *raw_text = NULL;
    size_t value_index;
    size_t array_index;
    size_t nested_index;
    size_t element_index;
    size_t element_count;
    uint64_t unsigned_value;
    double floating_value;
    json_boolean_t predicate;

    if (json_document_init(&document) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_document_parse_text(source, &document, error, sizeof(error)) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }

    if (json_object_get(&document, 0U, "n", &value_index) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if ((json_token_to_u64(&document, value_index, &unsigned_value) !=
        JSON_STATUS_SUCCESS) || (unsigned_value != UINT64_C(42))) {
        return TEST_STATUS_FAILURE;
    }

    if (json_object_get(&document, 0U, "f", &value_index) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if ((json_token_to_double(&document, value_index, &floating_value) !=
        JSON_STATUS_SUCCESS) || (floating_value != 1.5)) {
        return TEST_STATUS_FAILURE;
    }

    if (json_object_get(&document, 0U, "ok", &value_index) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if ((json_token_to_boolean(&document, value_index, &predicate) !=
        JSON_STATUS_SUCCESS) || (predicate != JSON_BOOLEAN_TRUE)) {
        return TEST_STATUS_FAILURE;
    }

    if (json_object_get(&document, 0U, "escaped", &value_index) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_token_copy_text(&document, value_index, &raw_text) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (strcmp(raw_text, "a\\n") != 0) {
        (void)json_memory_free(raw_text);
        return TEST_STATUS_FAILURE;
    }
    if (json_memory_free(raw_text) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    raw_text = NULL;

    if (json_object_get(&document, 0U, "items", &array_index) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if ((json_token_has_type(&document, array_index, JSON_TOKEN_ARRAY, &predicate) !=
        JSON_STATUS_SUCCESS) || (predicate != JSON_BOOLEAN_TRUE)) {
        return TEST_STATUS_FAILURE;
    }
    if ((json_array_size(&document, array_index, &element_count) !=
        JSON_STATUS_SUCCESS) || (element_count != 3U)) {
        return TEST_STATUS_FAILURE;
    }

    if (json_array_get(&document, array_index, 0U, &element_index) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_token_copy_text(&document, element_index, &raw_text) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (strcmp(raw_text, "x") != 0) {
        (void)json_memory_free(raw_text);
        return TEST_STATUS_FAILURE;
    }
    if (json_memory_free(raw_text) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    raw_text = NULL;

    if (json_array_get(&document, array_index, 1U, &nested_index) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_object_get(&document, nested_index, "inner", &value_index) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if ((json_token_to_u64(&document, value_index, &unsigned_value) !=
        JSON_STATUS_SUCCESS) || (unsigned_value != UINT64_C(2))) {
        return TEST_STATUS_FAILURE;
    }

    if (json_array_get(&document, array_index, 3U, &element_index) !=
        JSON_STATUS_NOT_FOUND) {
        return TEST_STATUS_FAILURE;
    }
    if (json_object_get(&document, 0U, "missing", &value_index) !=
        JSON_STATUS_NOT_FOUND) {
        return TEST_STATUS_FAILURE;
    }

    return (json_document_destroy(&document) == JSON_STATUS_SUCCESS) ?
        TEST_STATUS_SUCCESS : TEST_STATUS_FAILURE;
}

/**
 * @brief Проверяет строгую grammar и error-status контракты parser-а.
 * @return Именованный `test_status_t`.
 * @details
 * Сценарий проходит набор malformed object inputs, запрещённый root array,
 * некорректные numeric conversions, ошибка wrong-token-type и error-path file
 * loader-а. Между parse calls используется один инициализированный document, что
 * одновременно проверяет cleanup прежнего parse state.
 */
static test_status_t test_syntax_and_argument_errors(void)
{
    static const char *const malformed_sources[] = {
        "{",
        "{\"a\":}",
        "{\"a\":01}",
        "{\"a\":1,}",
        "{\"a\":[1,]}",
        "{\"a\":\"unterminated}",
        "[1,2,3]"
    };
    json_document_t document;
    char error[128];
    size_t source_index;
    size_t value_index;
    uint64_t unsigned_value;
    json_boolean_t boolean_value;

    if (json_document_init(&document) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }

    for (source_index = 0U;
        source_index < (sizeof(malformed_sources) / sizeof(malformed_sources[0]));
        source_index += 1U) {
        if (json_document_parse_text(malformed_sources[source_index], &document,
            error, sizeof(error)) != JSON_STATUS_SYNTAX_ERROR) {
            return TEST_STATUS_FAILURE;
        }
    }

    if (json_document_parse_text("{\"negative\":-1,\"fraction\":1.5}",
        &document, error, sizeof(error)) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_object_get(&document, 0U, "negative", &value_index) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_token_to_u64(&document, value_index, &unsigned_value) !=
        JSON_STATUS_SYNTAX_ERROR) {
        return TEST_STATUS_FAILURE;
    }
    if (json_object_get(&document, 0U, "fraction", &value_index) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_token_to_boolean(&document, value_index, &boolean_value) !=
        JSON_STATUS_SYNTAX_ERROR) {
        return TEST_STATUS_FAILURE;
    }
    if (json_array_size(&document, value_index, &source_index) !=
        JSON_STATUS_ARGUMENT_ERROR) {
        return TEST_STATUS_FAILURE;
    }
    if (json_document_parse_text(NULL, &document, error, sizeof(error)) !=
        JSON_STATUS_ARGUMENT_ERROR) {
        return TEST_STATUS_FAILURE;
    }
    if (json_document_load_file("build/no_such_json_lib_input.json", &document,
        error, sizeof(error)) != JSON_STATUS_IO_ERROR) {
        return TEST_STATUS_FAILURE;
    }

    return (json_document_destroy(&document) == JSON_STATUS_SUCCESS) ?
        TEST_STATUS_SUCCESS : TEST_STATUS_FAILURE;
}

/**
 * @brief Проверяет capacity retry при документе с числом токенов больше 64.
 * @return Именованный `test_status_t`.
 * @details
 * Сценарий программно строит object с array из 80 элементов. Первый parser pass
 * не помещается в начальный token array из 64 slots, поэтому успешный итог
 * доказывает корректность повторного pass с удвоенной ёмкостью.
 */
static test_status_t test_token_capacity_growth(void)
{
    char source[512];
    char error[128];
    json_document_t document;
    size_t array_index;
    size_t element_count;
    size_t offset = 0U;
    size_t value;

    offset = (size_t)snprintf(source, sizeof(source), "{\"items\":[");
    if (offset >= sizeof(source)) {
        return TEST_STATUS_FAILURE;
    }

    for (value = 0U; value < 80U; value += 1U) {
        int written = snprintf(source + offset, sizeof(source) - offset,
            "%s%zu", (value == 0U) ? "" : ",", value);
        if ((written < 0) || ((size_t)written >= (sizeof(source) - offset))) {
            return TEST_STATUS_FAILURE;
        }
        offset += (size_t)written;
    }

    if ((sizeof(source) - offset) < 3U) {
        return TEST_STATUS_FAILURE;
    }
    memcpy(source + offset, "]}", 3U);

    if (json_document_init(&document) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_document_parse_text(source, &document, error, sizeof(error)) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_object_get(&document, 0U, "items", &array_index) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if ((json_array_size(&document, array_index, &element_count) !=
        JSON_STATUS_SUCCESS) || (element_count != 80U)) {
        return TEST_STATUS_FAILURE;
    }

    return (json_document_destroy(&document) == JSON_STATUS_SUCCESS) ?
        TEST_STATUS_SUCCESS : TEST_STATUS_FAILURE;
}

/**
 * @brief Проверяет writer encoding, commit и subsequent document_load_file.
 * @return Именованный `test_status_t`.
 * @details
 * Сценарий создаёт transaction, комбинирует trusted punctuation с безопасным
 * string encoder-ом, публикует файл, затем разбирает опубликованное содержимое
 * через публичный file API. Завершающее удаление test artifact не является частью
 * json-lib API и предотвращает зависимость следующих запусков от предыдущих.
 */
static test_status_t test_atomic_writer_and_file_load(void)
{
    static const char output_path[] = "build/json_lib_test_output.json";
    json_writer_t writer = {0};
    json_document_t document;
    char error[128];
    char *text = NULL;
    size_t value_index;

    (void)remove(output_path);
    if (json_writer_open(output_path, &writer, error, sizeof(error)) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_writer_write_raw(&writer, "{\"message\":") != JSON_STATUS_SUCCESS) {
        (void)json_writer_abort(&writer);
        return TEST_STATUS_FAILURE;
    }
    if (json_writer_write_string(&writer, "quote=\" slash=\\ line=\n") !=
        JSON_STATUS_SUCCESS) {
        (void)json_writer_abort(&writer);
        return TEST_STATUS_FAILURE;
    }
    if (json_writer_write_raw(&writer, "}") != JSON_STATUS_SUCCESS) {
        (void)json_writer_abort(&writer);
        return TEST_STATUS_FAILURE;
    }
    if (json_writer_commit(&writer, error, sizeof(error)) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }

    if (json_document_init(&document) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_document_load_file(output_path, &document, error, sizeof(error)) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_object_get(&document, 0U, "message", &value_index) !=
        JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (json_token_copy_text(&document, value_index, &text) != JSON_STATUS_SUCCESS) {
        return TEST_STATUS_FAILURE;
    }
    if (strcmp(text, "quote=\\\" slash=\\\\ line=\\u000a") != 0) {
        (void)json_memory_free(text);
        return TEST_STATUS_FAILURE;
    }
    if ((json_memory_free(text) != JSON_STATUS_SUCCESS) ||
        (json_document_destroy(&document) != JSON_STATUS_SUCCESS)) {
        return TEST_STATUS_FAILURE;
    }

    (void)remove(output_path);
    return TEST_STATUS_SUCCESS;
}

/**
 * @brief Выполняет все независимые deterministic test scenarios.
 * @param result Output-статус первого обнаруженного сбоя либо общего успеха.
 * @return Именованный `test_status_t`.
 * @details
 * Алгоритм выполняет сценарии в фиксированном порядке. Параметр `result` делает
 * данные результата явными, а return type отделяет сбой инфраструктуры runner-а
 * от проверяемого library contract.
 */
static test_status_t test_run_all(test_status_t *result)
{
    if (result == NULL) {
        return TEST_STATUS_FAILURE;
    }

    *result = test_parse_navigation_and_conversion();
    if (*result != TEST_STATUS_SUCCESS) {
        return TEST_STATUS_SUCCESS;
    }

    *result = test_syntax_and_argument_errors();
    if (*result != TEST_STATUS_SUCCESS) {
        return TEST_STATUS_SUCCESS;
    }

    *result = test_token_capacity_growth();
    if (*result != TEST_STATUS_SUCCESS) {
        return TEST_STATUS_SUCCESS;
    }

    *result = test_atomic_writer_and_file_load();
    return TEST_STATUS_SUCCESS;
}

/**
 * @brief Преобразует named test result в допустимый ISO C process exit code.
 * @details
 * Единственная функция файла с `int` return type является required hosted C
 * entry point. Вся прикладная тестовая логика возвращает `test_status_t`.
 */
int main(void)
{
    test_status_t result;

    if (test_run_all(&result) != TEST_STATUS_SUCCESS) {
        return (int)TEST_STATUS_FAILURE;
    }

    return (int)result;
}

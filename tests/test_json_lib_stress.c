/**
 * @file test_json_lib_stress.c
 * @brief Повторяемая stress-проверка независимого parser ownership lifecycle.
 * @details
 * Тест выполняет 10 000 parse/navigate/convert/destroy транзакций с новым
 * `json_document_t` на каждой итерации. Он предназначен для ASan/UBSan и
 * Valgrind-проверок use-after-free, leaks и ошибок повторного освобождения без
 * смешения состояния одной итерации с другой.
 *
 * @par QG-13.f coverage
 * Покрываются массовое выделение и освобождение source/token memory, object
 * navigation, primitive conversion и возвращение document в нулевое состояние.
 */
#include "json_lib.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * @brief Представляет именованный итог stress scenario.
 * @details
 * Тип используется всеми helper-функциями; только ISO C `main` преобразует его
 * в `int` process exit code.
 */
typedef enum {
    STRESS_STATUS_SUCCESS = 0, /**< Все iterations успешно завершили lifecycle. */
    STRESS_STATUS_FAILURE = 1  /**< Хотя бы одна parser/navigation операция не удалась. */
} stress_status_t;

/**
 * @brief Выполняет одну изолированную parse/navigate/convert/destroy транзакцию.
 * @param iteration Номер сценария, включаемый в проверяемое numeric value.
 * @param result Output-итог транзакции.
 * @return Именованный `stress_status_t`.
 * @details
 * Алгоритм форматирует малый root object с переменным unsigned number, разбирает
 * документ, извлекает field `value`, преобразует его в `uint64_t`, сравнивает с
 * номером итерации и обязательно разрушает document до выхода из сценария.
 */
static stress_status_t stress_run_one(size_t iteration, stress_status_t *result)
{
    char source[64];
    char error[128];
    json_document_t document;
    size_t value_index;
    uint64_t value;
    int written;

    if (result == NULL) {
        return STRESS_STATUS_FAILURE;
    }

    written = snprintf(source, sizeof(source), "{\"value\":%zu}", iteration);
    if ((written < 0) || ((size_t)written >= sizeof(source))) {
        *result = STRESS_STATUS_FAILURE;
        return STRESS_STATUS_SUCCESS;
    }

    if (json_document_init(&document) != JSON_STATUS_SUCCESS) {
        *result = STRESS_STATUS_FAILURE;
        return STRESS_STATUS_SUCCESS;
    }
    if (json_document_parse_text(source, &document, error, sizeof(error)) !=
        JSON_STATUS_SUCCESS) {
        *result = STRESS_STATUS_FAILURE;
        return STRESS_STATUS_SUCCESS;
    }
    if (json_object_get(&document, 0U, "value", &value_index) !=
        JSON_STATUS_SUCCESS) {
        (void)json_document_destroy(&document);
        *result = STRESS_STATUS_FAILURE;
        return STRESS_STATUS_SUCCESS;
    }
    if ((json_token_to_u64(&document, value_index, &value) != JSON_STATUS_SUCCESS) ||
        (value != (uint64_t)iteration)) {
        (void)json_document_destroy(&document);
        *result = STRESS_STATUS_FAILURE;
        return STRESS_STATUS_SUCCESS;
    }
    if (json_document_destroy(&document) != JSON_STATUS_SUCCESS) {
        *result = STRESS_STATUS_FAILURE;
        return STRESS_STATUS_SUCCESS;
    }

    *result = STRESS_STATUS_SUCCESS;
    return STRESS_STATUS_SUCCESS;
}

/**
 * @brief Выполняет десять тысяч independent parser transactions.
 * @param result Output-итог полного stress scenario.
 * @return Именованный `stress_status_t`.
 * @details
 * Алгоритм вызывает `stress_run_one()` для последовательных 0..9999 значений и
 * немедленно завершает loop после первого failure. Успешный итог подтверждает,
 * что каждая итерация освобождает все owned resources до следующей.
 */
static stress_status_t stress_run_all(stress_status_t *result)
{
    size_t iteration;

    if (result == NULL) {
        return STRESS_STATUS_FAILURE;
    }

    for (iteration = 0U; iteration < 10000U; iteration += 1U) {
        if (stress_run_one(iteration, result) != STRESS_STATUS_SUCCESS) {
            return STRESS_STATUS_FAILURE;
        }
        if (*result != STRESS_STATUS_SUCCESS) {
            return STRESS_STATUS_SUCCESS;
        }
    }

    *result = STRESS_STATUS_SUCCESS;
    return STRESS_STATUS_SUCCESS;
}

/**
 * @brief Преобразует named stress result в допустимый ISO C exit code.
 * @details
 * Это единственная простая целочисленная return boundary test executable-а;
 * вспомогательные функции файла используют только `stress_status_t` и outputs.
 */
int main(void)
{
    stress_status_t result;

    if (stress_run_all(&result) != STRESS_STATUS_SUCCESS) {
        return (int)STRESS_STATUS_FAILURE;
    }

    return (int)result;
}

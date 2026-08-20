/**
 * @file bench_json_lib.c
 * @brief Однопоточный benchmark parser/navigation/conversion пути json-lib.
 * @details
 * Исполняемый файл строит один детерминированный JSON object указанной nesting
 * depth, многократно разбирает его, находит field `metric` и преобразует value
 * в `uint64_t`. Успешный запуск всегда печатает одну machine-readable строку
 * `benchmark=...`, после которой непосредственно следует обязательный marker
 * `Benchmark finished.`. Этот порядок совместим с проверками Makefile.
 *
 * Benchmark намеренно измеряет полный lifecycle parse/navigate/convert/destroy.
 * Он не предназначен для сравнения собственно JSON serialization, файлового I/O
 * или concurrent sharing одного document между потоками.
 */
#include "json_lib.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Представляет именованный итог benchmark helper-функций.
 * @details
 * Результат передаётся через named enum, чтобы только hosted ISO C entry point
 * `main` возвращала простой `int` process exit code.
 */
typedef enum {
    BENCH_STATUS_SUCCESS = 0, /**< Benchmark завершён и напечатал полный protocol. */
    BENCH_STATUS_FAILURE = 1  /**< CLI, allocation или json-lib операция завершилась ошибкой. */
} bench_status_t;

/**
 * @brief Хранит проверяемую конфигурацию одного benchmark запуска.
 * @details
 * Значения поступают из CLI либо documented defaults. `documents` определяет
 * число полных lifecycle итераций, `depth` — число добавленных object levels
 * между root и измеряемым `metric` field.
 */
typedef struct {
    size_t documents; /**< Число parser lifecycle транзакций; строго больше нуля. */
    size_t depth;     /**< Число nested object levels; допускается ноль. */
} json_benchmark_config_t;

/**
 * @brief Преобразует positive decimal CLI argument в `size_t`.
 * @param text Входная NUL-terminated decimal string.
 * @param value Output-положительное значение.
 * @return Именованный `bench_status_t`.
 * @details
 * Алгоритм применяет `strtoull()`, требует полного потребления input и запрещает
 * нулевое значение. Дополнительно выполняется round-trip cast проверка, чтобы
 * значение не обрезалось на платформе с более узким `size_t`.
 */
static bench_status_t bench_parse_positive_size(const char *text, size_t *value)
{
    char *end_pointer;
    unsigned long long parsed;

    if ((text == NULL) || (value == NULL)) {
        return BENCH_STATUS_FAILURE;
    }

    parsed = strtoull(text, &end_pointer, 10);
    if ((end_pointer == text) || (*end_pointer != '\0') || (parsed == 0ULL) ||
        ((unsigned long long)(size_t)parsed != parsed)) {
        return BENCH_STATUS_FAILURE;
    }

    *value = (size_t)parsed;
    return BENCH_STATUS_SUCCESS;
}

/**
 * @brief Разбирает documented CLI options benchmark executable-а.
 * @param argument_count Количество аргументов из `main`.
 * @param arguments Массив аргументов из `main`.
 * @param config Output-конфигурация benchmark запуска.
 * @return Именованный `bench_status_t`.
 * @details
 * Поддерживаются только пары `--documents N` и `--depth N`. Неизвестная опция,
 * пропущенное значение либо неположительное число отклоняются до allocation и
 * запуска benchmark цикла.
 */
static bench_status_t bench_parse_arguments(int argument_count, char **arguments,
    json_benchmark_config_t *config)
{
    int argument_index;

    if ((arguments == NULL) || (config == NULL)) {
        return BENCH_STATUS_FAILURE;
    }

    *config = (json_benchmark_config_t){
        .documents = 10000U,
        .depth = 8U
    };

    for (argument_index = 1; argument_index < argument_count;
        argument_index += 1) {
        if ((strcmp(arguments[argument_index], "--documents") == 0) &&
            ((argument_index + 1) < argument_count)) {
            argument_index += 1;
            if (bench_parse_positive_size(arguments[argument_index],
                &config->documents) != BENCH_STATUS_SUCCESS) {
                return BENCH_STATUS_FAILURE;
            }
        } else if ((strcmp(arguments[argument_index], "--depth") == 0) &&
            ((argument_index + 1) < argument_count)) {
            argument_index += 1;
            if (bench_parse_positive_size(arguments[argument_index],
                &config->depth) != BENCH_STATUS_SUCCESS) {
                return BENCH_STATUS_FAILURE;
            }
        } else {
            return BENCH_STATUS_FAILURE;
        }
    }

    return BENCH_STATUS_SUCCESS;
}

/**
 * @brief Строит валидный root object с указанным числом nested object levels.
 * @param depth Число levels между root и leaf `metric` field.
 * @param text Output-owned JSON source string.
 * @return Именованный `bench_status_t`.
 * @details
 * Алгоритм сначала вычисляет точную длину шаблона `{\"next\":` и closing `}`
 * частей, затем выделяет один buffer. Он записывает корневой префикс, повторяет
 * nested префикс `depth` раз, добавляет `{\"metric\":1}` и закрывает контейнеры.
 * Caller освобождает successful output через json_memory_free().
 */
static bench_status_t bench_build_document(size_t depth, char **text)
{
    static const char root_prefix[] = "{\"root\":";
    static const char nested_prefix[] = "{\"next\":";
    static const char leaf[] = "{\"metric\":1}";
    size_t root_length = sizeof(root_prefix) - 1U;
    size_t nested_length = sizeof(nested_prefix) - 1U;
    size_t leaf_length = sizeof(leaf) - 1U;
    size_t total_length;
    size_t offset = 0U;
    size_t level;
    char *allocation;

    if (text == NULL) {
        return BENCH_STATUS_FAILURE;
    }
    if (depth > ((SIZE_MAX - root_length - leaf_length - 2U) /
        (nested_length + 1U))) {
        return BENCH_STATUS_FAILURE;
    }

    total_length = root_length + (depth * nested_length) + leaf_length + depth + 2U;
    allocation = malloc(total_length + 1U);
    if (allocation == NULL) {
        return BENCH_STATUS_FAILURE;
    }

    memcpy(allocation + offset, root_prefix, root_length);
    offset += root_length;
    for (level = 0U; level < depth; level += 1U) {
        memcpy(allocation + offset, nested_prefix, nested_length);
        offset += nested_length;
    }
    memcpy(allocation + offset, leaf, leaf_length);
    offset += leaf_length;
    for (level = 0U; level < (depth + 1U); level += 1U) {
        allocation[offset] = '}';
        offset += 1U;
    }
    allocation[offset] = '\0';

    *text = allocation;
    return BENCH_STATUS_SUCCESS;
}

/**
 * @brief Находит leaf metric через document root и последовательность `next` keys.
 * @param document Успешно разобранный benchmark document.
 * @param depth Число nested `next` levels.
 * @param metric_index Output-token index leaf metric value.
 * @return Именованный `bench_status_t`.
 * @details
 * Алгоритм получает `root` из root object, повторяет поиск `next` ровно depth раз
 * и затем запрашивает `metric`. Он является частью измеряемой навигационной
 * нагрузки и одновременно подтверждает правильность сформированного source.
 */
static bench_status_t bench_find_metric(const json_document_t *document,
    size_t depth, size_t *metric_index)
{
    size_t current_index;
    size_t level;

    if ((document == NULL) || (metric_index == NULL)) {
        return BENCH_STATUS_FAILURE;
    }

    if (json_object_get(document, 0U, "root", &current_index) !=
        JSON_STATUS_SUCCESS) {
        return BENCH_STATUS_FAILURE;
    }
    for (level = 0U; level < depth; level += 1U) {
        if (json_object_get(document, current_index, "next", &current_index) !=
            JSON_STATUS_SUCCESS) {
            return BENCH_STATUS_FAILURE;
        }
    }
    if (json_object_get(document, current_index, "metric", metric_index) !=
        JSON_STATUS_SUCCESS) {
        return BENCH_STATUS_FAILURE;
    }

    return BENCH_STATUS_SUCCESS;
}

/**
 * @brief Выполняет параметризованный benchmark и печатает стабильный protocol.
 * @param config Проверенная configuration.
 * @return Именованный `bench_status_t`.
 * @details
 * Алгоритм строит source один раз, затем для каждого документа выполняет полный
 * document init, parse, depth navigation, `uint64_t` conversion и destroy. Sink
 * суммирует значения и исключает устранение полезной работы оптимизатором. После
 * успеха печатаются ровно две final lines в требуемом порядке.
 */
static bench_status_t bench_run(const json_benchmark_config_t *config)
{
    char *source = NULL;
    size_t document_index;
    uint64_t sink = UINT64_C(0);

    if (config == NULL) {
        return BENCH_STATUS_FAILURE;
    }
    if (bench_build_document(config->depth, &source) != BENCH_STATUS_SUCCESS) {
        return BENCH_STATUS_FAILURE;
    }

    for (document_index = 0U; document_index < config->documents;
        document_index += 1U) {
        json_document_t document;
        char error[128];
        size_t metric_index;
        uint64_t metric_value;

        if (json_document_init(&document) != JSON_STATUS_SUCCESS) {
            (void)json_memory_free(source);
            return BENCH_STATUS_FAILURE;
        }
        if (json_document_parse_text(source, &document, error, sizeof(error)) !=
            JSON_STATUS_SUCCESS) {
            (void)json_document_destroy(&document);
            (void)json_memory_free(source);
            return BENCH_STATUS_FAILURE;
        }
        if (bench_find_metric(&document, config->depth, &metric_index) !=
            BENCH_STATUS_SUCCESS) {
            (void)json_document_destroy(&document);
            (void)json_memory_free(source);
            return BENCH_STATUS_FAILURE;
        }
        if (json_token_to_u64(&document, metric_index, &metric_value) !=
            JSON_STATUS_SUCCESS) {
            (void)json_document_destroy(&document);
            (void)json_memory_free(source);
            return BENCH_STATUS_FAILURE;
        }
        if (json_document_destroy(&document) != JSON_STATUS_SUCCESS) {
            (void)json_memory_free(source);
            return BENCH_STATUS_FAILURE;
        }

        sink += metric_value;
    }

    if (json_memory_free(source) != JSON_STATUS_SUCCESS) {
        return BENCH_STATUS_FAILURE;
    }

    (void)printf("benchmark=json_lib documents=%zu depth=%zu sink=%llu\n",
        config->documents, config->depth, (unsigned long long)sink);
    (void)printf("Benchmark finished.\n");
    return BENCH_STATUS_SUCCESS;
}

/**
 * @brief Преобразует benchmark status в допустимый ISO C process exit code.
 * @details
 * `main` является единственной функцией executable-а с простым `int` return
 * type. Все parsing/building/benchmark операции выше возвращают named status.
 */
int main(int argument_count, char **arguments)
{
    json_benchmark_config_t config;

    if (bench_parse_arguments(argument_count, arguments, &config) !=
        BENCH_STATUS_SUCCESS) {
        return (int)BENCH_STATUS_FAILURE;
    }

    return (int)bench_run(&config);
}

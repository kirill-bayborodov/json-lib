/**
 * @file json_lib.c
 * @brief Самостоятельная C11-реализация parser, navigator и atomic writer json-lib.
 * @details
 * Реализация использует recursive-descent разбор JSON-грамматики. Вместо дерева
 * создаётся префиксный массив `json_token_t`: каждый добавленный токен хранит
 * диапазон исходного текста и индекс своего контейнера. Этот формат делает
 * обход объектов и массивов детерминированным, не требует рекурсивных heap-
 * allocations и сохраняет raw representation primitive/string значений.
 *
 * Внутренние helper-функции также возвращают только `json_status_t`. Лексический
 * результат передаётся отдельным output-параметром `json_lex_status_t`, чтобы
 * отличить ошибку грамматики от ошибки аргумента, памяти либо I/O публичного API.
 */
#define _POSIX_C_SOURCE 200809L

#include "json_lib.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * @brief Описывает результат внутреннего разбора без смешения с public status.
 * @details
 * Парсер возвращает `JSON_STATUS_SUCCESS`, когда его собственные аргументы и
 * allocations корректны. Поле этого типа сообщает вызывающему коду, завершился
 * ли разбор успешно, обнаружил syntax error или упёрся в caller-provided
 * token capacity.
 */
typedef enum {
    JSON_LEX_STATUS_SUCCESS = 0,  /**< Разбор текущего фрагмента завершён успешно. */
    JSON_LEX_STATUS_SYNTAX = 1,   /**< Вход не удовлетворяет JSON-грамматике. */
    JSON_LEX_STATUS_CAPACITY = 2  /**< Массив токенов слишком мал для документа. */
} json_lex_status_t;

/**
 * @brief Хранит неизменяемый input и состояние одного bounded parser pass.
 * @details
 * Структура живёт на стеке `json_document_parse_text()`. В ней нет ownership
 * исходного текста или token array: памятью владеет вызывающий документ, а parser
 * только добавляет в уже выделенный массив token records.
 */
typedef struct {
    const char *text;        /**< Неизменяемый NUL-terminated JSON source. */
    size_t text_size;        /**< Длина source без завершающего NUL. */
    json_token_t *tokens;    /**< Caller-owned массив для добавляемых токенов. */
    size_t token_capacity;   /**< Максимальное число записываемых токенов. */
    size_t token_count;      /**< Число токенов, уже добавленных в этот pass. */
} json_parser_t;

/**
 * @brief Сохраняет ограниченную диагностическую строку, не меняя primary status.
 * @param error Nullable output-буфер.
 * @param error_capacity Ёмкость буфера в байтах.
 * @param format Формат строки, совместимый с `vsnprintf()`.
 * @return `JSON_STATUS_SUCCESS`.
 * @details
 * Алгоритм записывает диагностику только когда передан ненулевой буфер ненулевой
 * ёмкости. Ошибка форматирования намеренно не заменяет основную причину ошибки
 * JSON/I/O, которую возвращает вызывающая операция.
 */
static json_status_t json_set_error(char *error, size_t error_capacity,
    const char *format, ...)
{
    va_list arguments;

    if ((error != NULL) && (error_capacity > 0U) && (format != NULL)) {
        va_start(arguments, format);
        (void)vsnprintf(error, error_capacity, format, arguments);
        va_end(arguments);
    }

    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Копирует NUL-terminated строку в owned allocation.
 * @param source Входная строка для копирования.
 * @param copy Output-указатель на новую строку.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм вычисляет длину, проверяет переполнение для завершающего NUL, выделяет
 * ровно нужное число байтов и копирует строку через `memcpy()`. Helper не
 * использует POSIX `strdup()`, поэтому проверка размера и ownership остаются
 * явными в одном месте.
 */
static json_status_t json_duplicate_string(const char *source, char **copy)
{
    char *allocation;
    size_t length;

    if ((source == NULL) || (copy == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    length = strlen(source);
    if (length == SIZE_MAX) {
        return JSON_STATUS_CAPACITY_ERROR;
    }

    allocation = malloc(length + 1U);
    if (allocation == NULL) {
        return JSON_STATUS_ALLOCATION_ERROR;
    }

    memcpy(allocation, source, length + 1U);
    *copy = allocation;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Пропускает JSON whitespace начиная с указанной позиции.
 * @param parser Parser с проверяемым исходным текстом.
 * @param position Input/output смещение в source.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм пропускает только четыре JSON whitespace byte: space, tab, CR и LF.
 * Другие пробельные символы C locale не принимаются JSON-стандартом и остаются
 * для последующей syntax validation.
 */
static json_status_t json_skip_whitespace(const json_parser_t *parser,
    size_t *position)
{
    if ((parser == NULL) || (position == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    while ((*position < parser->text_size) &&
        ((parser->text[*position] == ' ') ||
         (parser->text[*position] == '\t') ||
         (parser->text[*position] == '\r') ||
         (parser->text[*position] == '\n'))) {
        *position += 1U;
    }

    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Добавляет один token в префиксный parser array.
 * @param parser Parser, владеющий текущим счётчиком и capacity.
 * @param type Синтаксическая категория нового токена.
 * @param start Включаемое смещение начала токена.
 * @param parent_index Индекс контейнера-родителя или `-1` для root.
 * @param token_index Output-индекс нового токена.
 * @param lex_status Output-результат capacity validation.
 * @return Именованный `json_status_t`.
 * @details
 * При наличии свободного слота helper инициализирует все поля token record и
 * увеличивает `child_count` непосредственного родителя. При исчерпании capacity
 * token array не меняется, а вызывающий код получает
 * `JSON_LEX_STATUS_CAPACITY` и может повторить весь pass с большей ёмкостью.
 */
static json_status_t json_parser_append(json_parser_t *parser,
    json_token_type_t type, size_t start, ptrdiff_t parent_index,
    size_t *token_index, json_lex_status_t *lex_status)
{
    json_token_t *token;

    if ((parser == NULL) || (token_index == NULL) || (lex_status == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    if (parser->token_count >= parser->token_capacity) {
        *lex_status = JSON_LEX_STATUS_CAPACITY;
        return JSON_STATUS_SUCCESS;
    }

    token = &parser->tokens[parser->token_count];
    token->type = type;
    token->start = start;
    token->end = start;
    token->child_count = 0U;
    token->parent_index = parent_index;
    *token_index = parser->token_count;
    parser->token_count += 1U;

    if (parent_index >= 0) {
        parser->tokens[parent_index].child_count += 1U;
    }

    *lex_status = JSON_LEX_STATUS_SUCCESS;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Проверяет и разбирает одну JSON string token.
 * @param parser Parser с исходным текстом и token array.
 * @param position Смещение открывающей кавычки во входе.
 * @param parent_index Контейнер string token.
 * @param next_position Output-смещение сразу после закрывающей кавычки.
 * @param lex_status Output-состояние syntax/capacity проверки.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм сканирует bytes между внешними кавычками, отклоняет неэкранированные
 * control bytes и проверяет допустимые short escapes. Escape `\\u` требует ровно
 * четыре hexadecimal digits. В token сохраняется диапазон без внешних кавычек.
 */
static json_status_t json_parse_string(json_parser_t *parser, size_t position,
    ptrdiff_t parent_index, size_t *next_position, json_lex_status_t *lex_status)
{
    size_t cursor;
    size_t token_index;
    json_status_t status;

    if ((parser == NULL) || (next_position == NULL) || (lex_status == NULL) ||
        (position >= parser->text_size) || (parser->text[position] != '"')) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    cursor = position + 1U;
    while (cursor < parser->text_size) {
        unsigned char byte = (unsigned char)parser->text[cursor];

        if (byte == '"') {
            status = json_parser_append(parser, JSON_TOKEN_STRING, position + 1U,
                parent_index, &token_index, lex_status);
            if ((status != JSON_STATUS_SUCCESS) ||
                (*lex_status != JSON_LEX_STATUS_SUCCESS)) {
                return status;
            }

            parser->tokens[token_index].end = cursor;
            *next_position = cursor + 1U;
            return JSON_STATUS_SUCCESS;
        }

        if (byte < 0x20U) {
            *lex_status = JSON_LEX_STATUS_SYNTAX;
            return JSON_STATUS_SUCCESS;
        }

        if (byte == '\\') {
            size_t hex_index;

            cursor += 1U;
            if (cursor >= parser->text_size) {
                *lex_status = JSON_LEX_STATUS_SYNTAX;
                return JSON_STATUS_SUCCESS;
            }

            if (strchr("\"\\\\/bfnrt", parser->text[cursor]) != NULL) {
                cursor += 1U;
                continue;
            }

            if (parser->text[cursor] != 'u') {
                *lex_status = JSON_LEX_STATUS_SYNTAX;
                return JSON_STATUS_SUCCESS;
            }

            for (hex_index = 0U; hex_index < 4U; hex_index += 1U) {
                cursor += 1U;
                if ((cursor >= parser->text_size) ||
                    !isxdigit((unsigned char)parser->text[cursor])) {
                    *lex_status = JSON_LEX_STATUS_SYNTAX;
                    return JSON_STATUS_SUCCESS;
                }
            }
        }

        cursor += 1U;
    }

    *lex_status = JSON_LEX_STATUS_SYNTAX;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Проверяет JSON number grammar и добавляет primitive token.
 * @param parser Parser с исходным текстом и token array.
 * @param position Смещение первого символа number.
 * @param parent_index Контейнер primitive token.
 * @param next_position Output-смещение сразу после number.
 * @param lex_status Output-состояние syntax/capacity проверки.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм реализует JSON number grammar: optional `-`, затем `0` либо nonzero
 * integer, optional fractional часть и optional exponent. Он не делегирует
 * проверку `strtod()`, поскольку JSON запрещает `+1`, leading zero и special
 * floating-point literals.
 */
static json_status_t json_parse_number(json_parser_t *parser, size_t position,
    ptrdiff_t parent_index, size_t *next_position, json_lex_status_t *lex_status)
{
    size_t cursor;
    size_t token_index;
    json_status_t status;

    if ((parser == NULL) || (next_position == NULL) || (lex_status == NULL) ||
        (position >= parser->text_size)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    cursor = position;
    if (parser->text[cursor] == '-') {
        cursor += 1U;
        if (cursor >= parser->text_size) {
            *lex_status = JSON_LEX_STATUS_SYNTAX;
            return JSON_STATUS_SUCCESS;
        }
    }

    if (parser->text[cursor] == '0') {
        cursor += 1U;
    } else if ((parser->text[cursor] >= '1') && (parser->text[cursor] <= '9')) {
        cursor += 1U;
        while ((cursor < parser->text_size) &&
            isdigit((unsigned char)parser->text[cursor])) {
            cursor += 1U;
        }
    } else {
        *lex_status = JSON_LEX_STATUS_SYNTAX;
        return JSON_STATUS_SUCCESS;
    }

    if ((cursor < parser->text_size) && (parser->text[cursor] == '.')) {
        cursor += 1U;
        if ((cursor >= parser->text_size) ||
            !isdigit((unsigned char)parser->text[cursor])) {
            *lex_status = JSON_LEX_STATUS_SYNTAX;
            return JSON_STATUS_SUCCESS;
        }

        while ((cursor < parser->text_size) &&
            isdigit((unsigned char)parser->text[cursor])) {
            cursor += 1U;
        }
    }

    if ((cursor < parser->text_size) &&
        ((parser->text[cursor] == 'e') || (parser->text[cursor] == 'E'))) {
        cursor += 1U;
        if ((cursor < parser->text_size) &&
            ((parser->text[cursor] == '+') || (parser->text[cursor] == '-'))) {
            cursor += 1U;
        }

        if ((cursor >= parser->text_size) ||
            !isdigit((unsigned char)parser->text[cursor])) {
            *lex_status = JSON_LEX_STATUS_SYNTAX;
            return JSON_STATUS_SUCCESS;
        }

        while ((cursor < parser->text_size) &&
            isdigit((unsigned char)parser->text[cursor])) {
            cursor += 1U;
        }
    }

    status = json_parser_append(parser, JSON_TOKEN_PRIMITIVE, position,
        parent_index, &token_index, lex_status);
    if ((status != JSON_STATUS_SUCCESS) ||
        (*lex_status != JSON_LEX_STATUS_SUCCESS)) {
        return status;
    }

    parser->tokens[token_index].end = cursor;
    *next_position = cursor;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Проверяет JSON literal `true`, `false` либо `null` и добавляет token.
 * @param parser Parser с исходным текстом и token array.
 * @param position Смещение первого символа literal.
 * @param parent_index Контейнер primitive token.
 * @param literal Ожидаемая NUL-terminated JSON literal.
 * @param next_position Output-смещение сразу после literal.
 * @param lex_status Output-состояние syntax/capacity проверки.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм сравнивает literal с исходным текстом без чтения за конец source и
 * добавляет token только при полном точном совпадении. Проверку разделителя
 * выполняет caller `json_parse_value()` через структуру object/array grammar.
 */
static json_status_t json_parse_literal(json_parser_t *parser, size_t position,
    ptrdiff_t parent_index, const char *literal, size_t *next_position,
    json_lex_status_t *lex_status)
{
    size_t length;
    size_t token_index;
    json_status_t status;

    if ((parser == NULL) || (literal == NULL) || (next_position == NULL) ||
        (lex_status == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    length = strlen(literal);
    if ((position > parser->text_size) ||
        (length > (parser->text_size - position)) ||
        (memcmp(parser->text + position, literal, length) != 0)) {
        *lex_status = JSON_LEX_STATUS_SYNTAX;
        return JSON_STATUS_SUCCESS;
    }

    status = json_parser_append(parser, JSON_TOKEN_PRIMITIVE, position,
        parent_index, &token_index, lex_status);
    if ((status != JSON_STATUS_SUCCESS) ||
        (*lex_status != JSON_LEX_STATUS_SUCCESS)) {
        return status;
    }

    parser->tokens[token_index].end = position + length;
    *next_position = position + length;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Разбирает JSON value и добавляет все токены его subtree.
 * @param parser Parser с исходным текстом и token array.
 * @param position Смещение начала value после JSON whitespace.
 * @param parent_index Контейнер value или `-1` для root.
 * @param next_position Output-смещение сразу после value.
 * @param lex_status Output-состояние syntax/capacity проверки.
 * @return Именованный `json_status_t`.
 * @details
 * Helper диспетчеризует значение по первому byte: string, object, array, literal
 * или number. Рекурсивные варианты вызывают object/array helper, определённые
 * ниже; прототипы располагаются перед этой функцией, чтобы сохранить явный
 * порядок зависимостей алгоритма.
 */
static json_status_t json_parse_value(json_parser_t *parser, size_t position,
    ptrdiff_t parent_index, size_t *next_position, json_lex_status_t *lex_status);

/**
 * @brief Разбирает JSON object, включая строгую последовательность key/value пар.
 * @param parser Parser с исходным текстом и token array.
 * @param position Смещение открывающей `{`.
 * @param parent_index Контейнер object или `-1` для root.
 * @param next_position Output-смещение сразу после закрывающей `}`.
 * @param lex_status Output-состояние syntax/capacity проверки.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм добавляет object token до его детей, затем разбирает ноль или более
 * последовательностей `string ':' value`, разделённых запятой. Отсутствующий
 * colon, лишняя запятая, non-string key и unterminated object переводятся в
 * `JSON_LEX_STATUS_SYNTAX` без публикации частичного document.
 */
static json_status_t json_parse_object(json_parser_t *parser, size_t position,
    ptrdiff_t parent_index, size_t *next_position, json_lex_status_t *lex_status)
{
    size_t cursor;
    size_t object_index;
    json_status_t status;

    if ((parser == NULL) || (next_position == NULL) || (lex_status == NULL) ||
        (position >= parser->text_size) || (parser->text[position] != '{')) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_parser_append(parser, JSON_TOKEN_OBJECT, position, parent_index,
        &object_index, lex_status);
    if ((status != JSON_STATUS_SUCCESS) ||
        (*lex_status != JSON_LEX_STATUS_SUCCESS)) {
        return status;
    }

    cursor = position + 1U;
    status = json_skip_whitespace(parser, &cursor);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    if ((cursor < parser->text_size) && (parser->text[cursor] == '}')) {
        parser->tokens[object_index].end = cursor + 1U;
        *next_position = cursor + 1U;
        return JSON_STATUS_SUCCESS;
    }

    for (;;) {
        size_t key_end;
        size_t value_end;

        if ((cursor >= parser->text_size) || (parser->text[cursor] != '"')) {
            *lex_status = JSON_LEX_STATUS_SYNTAX;
            return JSON_STATUS_SUCCESS;
        }

        status = json_parse_string(parser, cursor, (ptrdiff_t)object_index,
            &key_end, lex_status);
        if ((status != JSON_STATUS_SUCCESS) ||
            (*lex_status != JSON_LEX_STATUS_SUCCESS)) {
            return status;
        }

        cursor = key_end;
        status = json_skip_whitespace(parser, &cursor);
        if (status != JSON_STATUS_SUCCESS) {
            return status;
        }

        if ((cursor >= parser->text_size) || (parser->text[cursor] != ':')) {
            *lex_status = JSON_LEX_STATUS_SYNTAX;
            return JSON_STATUS_SUCCESS;
        }

        cursor += 1U;
        status = json_skip_whitespace(parser, &cursor);
        if (status != JSON_STATUS_SUCCESS) {
            return status;
        }

        status = json_parse_value(parser, cursor, (ptrdiff_t)object_index,
            &value_end, lex_status);
        if ((status != JSON_STATUS_SUCCESS) ||
            (*lex_status != JSON_LEX_STATUS_SUCCESS)) {
            return status;
        }

        cursor = value_end;
        status = json_skip_whitespace(parser, &cursor);
        if (status != JSON_STATUS_SUCCESS) {
            return status;
        }

        if ((cursor < parser->text_size) && (parser->text[cursor] == ',')) {
            cursor += 1U;
            status = json_skip_whitespace(parser, &cursor);
            if (status != JSON_STATUS_SUCCESS) {
                return status;
            }
            continue;
        }

        if ((cursor < parser->text_size) && (parser->text[cursor] == '}')) {
            parser->tokens[object_index].end = cursor + 1U;
            *next_position = cursor + 1U;
            return JSON_STATUS_SUCCESS;
        }

        *lex_status = JSON_LEX_STATUS_SYNTAX;
        return JSON_STATUS_SUCCESS;
    }
}

/**
 * @brief Разбирает JSON array, включая разделённые запятыми value subtrees.
 * @param parser Parser с исходным текстом и token array.
 * @param position Смещение открывающей `[`.
 * @param parent_index Контейнер array или `-1` для root.
 * @param next_position Output-смещение сразу после закрывающей `]`.
 * @param lex_status Output-состояние syntax/capacity проверки.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм добавляет array token до его элементов. Он принимает пустой массив
 * либо одну или более values, разделённых одиночной запятой, и запрещает trailing
 * comma. Значения рекурсивно добавляют полные subtrees в префиксном порядке.
 */
static json_status_t json_parse_array(json_parser_t *parser, size_t position,
    ptrdiff_t parent_index, size_t *next_position, json_lex_status_t *lex_status)
{
    size_t cursor;
    size_t array_index;
    json_status_t status;

    if ((parser == NULL) || (next_position == NULL) || (lex_status == NULL) ||
        (position >= parser->text_size) || (parser->text[position] != '[')) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_parser_append(parser, JSON_TOKEN_ARRAY, position, parent_index,
        &array_index, lex_status);
    if ((status != JSON_STATUS_SUCCESS) ||
        (*lex_status != JSON_LEX_STATUS_SUCCESS)) {
        return status;
    }

    cursor = position + 1U;
    status = json_skip_whitespace(parser, &cursor);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    if ((cursor < parser->text_size) && (parser->text[cursor] == ']')) {
        parser->tokens[array_index].end = cursor + 1U;
        *next_position = cursor + 1U;
        return JSON_STATUS_SUCCESS;
    }

    for (;;) {
        size_t value_end;

        status = json_parse_value(parser, cursor, (ptrdiff_t)array_index,
            &value_end, lex_status);
        if ((status != JSON_STATUS_SUCCESS) ||
            (*lex_status != JSON_LEX_STATUS_SUCCESS)) {
            return status;
        }

        cursor = value_end;
        status = json_skip_whitespace(parser, &cursor);
        if (status != JSON_STATUS_SUCCESS) {
            return status;
        }

        if ((cursor < parser->text_size) && (parser->text[cursor] == ',')) {
            cursor += 1U;
            status = json_skip_whitespace(parser, &cursor);
            if (status != JSON_STATUS_SUCCESS) {
                return status;
            }
            continue;
        }

        if ((cursor < parser->text_size) && (parser->text[cursor] == ']')) {
            parser->tokens[array_index].end = cursor + 1U;
            *next_position = cursor + 1U;
            return JSON_STATUS_SUCCESS;
        }

        *lex_status = JSON_LEX_STATUS_SYNTAX;
        return JSON_STATUS_SUCCESS;
    }
}

/**
 * @brief Диспетчеризует JSON value по первому значимому byte.
 * @details
 * Алгоритм предполагает, что caller уже пропустил whitespace. Для object/array
 * он передаёт управление контейнерным helper-функциям, для string/literal/number
 * — leaf helper-функциям. Другой byte однозначно означает syntax error.
 */
static json_status_t json_parse_value(json_parser_t *parser, size_t position,
    ptrdiff_t parent_index, size_t *next_position, json_lex_status_t *lex_status)
{
    char first_byte;

    if ((parser == NULL) || (next_position == NULL) || (lex_status == NULL) ||
        (position >= parser->text_size)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    first_byte = parser->text[position];
    if (first_byte == '{') {
        return json_parse_object(parser, position, parent_index, next_position,
            lex_status);
    }
    if (first_byte == '[') {
        return json_parse_array(parser, position, parent_index, next_position,
            lex_status);
    }
    if (first_byte == '"') {
        return json_parse_string(parser, position, parent_index, next_position,
            lex_status);
    }
    if (first_byte == 't') {
        return json_parse_literal(parser, position, parent_index, "true",
            next_position, lex_status);
    }
    if (first_byte == 'f') {
        return json_parse_literal(parser, position, parent_index, "false",
            next_position, lex_status);
    }
    if (first_byte == 'n') {
        return json_parse_literal(parser, position, parent_index, "null",
            next_position, lex_status);
    }

    return json_parse_number(parser, position, parent_index, next_position,
        lex_status);
}

/**
 * @brief Выполняет один полный parser pass и возвращает число созданных токенов.
 * @param parser Подготовленное parser state.
 * @param token_count Output-число токенов после успешного pass.
 * @param lex_status Output-результат syntax/capacity проверки.
 * @return Именованный `json_status_t`.
 * @details
 * Алгоритм разбирает один root value, пропускает допускаемый trailing whitespace
 * и требует точного достижения конца input. Затем он требует root object согласно
 * public contract `json_document_parse_text()`. Такой финальный контроль
 * исключает допустимые JSON scalars/arrays как неподходящий document API input.
 */
static json_status_t json_parse_document(json_parser_t *parser,
    size_t *token_count, json_lex_status_t *lex_status)
{
    size_t cursor = 0U;
    size_t next_position;
    json_status_t status;

    if ((parser == NULL) || (token_count == NULL) || (lex_status == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    *lex_status = JSON_LEX_STATUS_SUCCESS;
    status = json_skip_whitespace(parser, &cursor);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    if (cursor >= parser->text_size) {
        *lex_status = JSON_LEX_STATUS_SYNTAX;
        return JSON_STATUS_SUCCESS;
    }

    status = json_parse_value(parser, cursor, -1, &next_position, lex_status);
    if ((status != JSON_STATUS_SUCCESS) ||
        (*lex_status != JSON_LEX_STATUS_SUCCESS)) {
        return status;
    }

    cursor = next_position;
    status = json_skip_whitespace(parser, &cursor);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    if ((cursor != parser->text_size) || (parser->token_count == 0U) ||
        (parser->tokens[0].type != JSON_TOKEN_OBJECT)) {
        *lex_status = JSON_LEX_STATUS_SYNTAX;
        return JSON_STATUS_SUCCESS;
    }

    *token_count = parser->token_count;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Валидирует token index и выдаёт read-only адрес token record.
 * @param document Успешно разобранный document.
 * @param token_index Индекс требуемого token.
 * @param token Output-read-only адрес token record.
 * @return `JSON_STATUS_SUCCESS` либо `JSON_STATUS_ARGUMENT_ERROR`.
 * @details
 * Алгоритм проверяет ownership-инварианты document и диапазон индекса до любого
 * разыменования. Все public navigation/conversion функции используют этот helper
 * как единый барьер от неинициализированных или разрушенных объектов.
 */
static json_status_t json_get_token(const json_document_t *document,
    size_t token_index, const json_token_t **token)
{
    if ((document == NULL) || (token == NULL) || (document->text == NULL) ||
        (document->tokens == NULL) || (token_index >= document->token_count)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    *token = &document->tokens[token_index];
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Находит индекс первого token сразу после subtree.
 * @param document Успешно разобранный document.
 * @param token_index Корень пропускаемого subtree.
 * @param end_index Output-позиция первого token после subtree.
 * @return Именованный `json_status_t`.
 * @details
 * Префиксный порядок гарантирует, что все потомки token имеют parent index не
 * меньше root index и идут непрерывным диапазоном. Helper линейно проходит этот
 * диапазон; он используется для перехода через объектное value или вложенный
 * array element без построения отдельного дерева.
 */
static json_status_t json_subtree_end(const json_document_t *document,
    size_t token_index, size_t *end_index)
{
    const json_token_t *token;
    json_status_t status;

    if (end_index == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, token_index, &token);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    (void)token;
    *end_index = token_index + 1U;
    while ((*end_index < document->token_count) &&
        (document->tokens[*end_index].parent_index >= (ptrdiff_t)token_index)) {
        *end_index += 1U;
    }

    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Инициализирует document как пустой объект без allocations.
 * @details
 * Алгоритм обнуляет aggregate; результат безопасен для destroy и последующего
 * parse/load. Неинициализированный указатель document не разыменовывается.
 */
json_status_t json_document_init(json_document_t *document)
{
    if (document == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    *document = (json_document_t){0};
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Освобождает owned text/tokens и возвращает document в пустое состояние.
 * @details
 * Алгоритм независимо освобождает оба heap-поля, после чего вызывает единый
 * initialization helper. Это делает повторный destroy идемпотентным по памяти.
 */
json_status_t json_document_destroy(json_document_t *document)
{
    if (document == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    free(document->text);
    free(document->tokens);
    return json_document_init(document);
}

/**
 * @brief Разбирает root-object JSON-text и публикует плоский document.
 * @details
 * Алгоритм освобождает прежнее содержимое document, копирует input и повторяет
 * bounded parser pass. При `JSON_LEX_STATUS_CAPACITY` токенный массив удваивается
 * и pass начинается заново; при syntax/memory error object остаётся пустым.
 */
json_status_t json_document_parse_text(const char *text, json_document_t *document,
    char *error, size_t error_capacity)
{
    size_t text_size;
    size_t token_capacity = 64U;
    size_t token_count = 0U;
    json_lex_status_t lex_status = JSON_LEX_STATUS_SYNTAX;
    json_status_t status;

    if ((text == NULL) || (document == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_document_destroy(document);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    text_size = strlen(text);
    if (text_size == SIZE_MAX) {
        return JSON_STATUS_CAPACITY_ERROR;
    }

    document->text = malloc(text_size + 1U);
    if (document->text == NULL) {
        return JSON_STATUS_ALLOCATION_ERROR;
    }

    memcpy(document->text, text, text_size + 1U);
    document->text_size = text_size;

    for (;;) {
        json_parser_t parser = {
            .text = document->text,
            .text_size = document->text_size,
            .tokens = NULL,
            .token_capacity = token_capacity,
            .token_count = 0U
        };

        document->tokens = calloc(token_capacity, sizeof(*document->tokens));
        if (document->tokens == NULL) {
            (void)json_document_destroy(document);
            return JSON_STATUS_ALLOCATION_ERROR;
        }

        parser.tokens = document->tokens;
        status = json_parse_document(&parser, &token_count, &lex_status);
        if (status != JSON_STATUS_SUCCESS) {
            (void)json_document_destroy(document);
            return status;
        }

        if (lex_status != JSON_LEX_STATUS_CAPACITY) {
            break;
        }

        free(document->tokens);
        document->tokens = NULL;
        if (token_capacity > (SIZE_MAX / 2U)) {
            (void)json_document_destroy(document);
            return JSON_STATUS_CAPACITY_ERROR;
        }
        token_capacity *= 2U;
    }

    if (lex_status != JSON_LEX_STATUS_SUCCESS) {
        (void)json_set_error(error, error_capacity,
            "input is not a JSON object accepted by json-lib");
        (void)json_document_destroy(document);
        return JSON_STATUS_SYNTAX_ERROR;
    }

    document->token_count = token_count;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Загружает input file и передаёт его в text parser.
 * @details
 * Алгоритм читает файл целиком в временный NUL-terminated allocation. После
 * вызова parser-а этот allocation освобождается независимо от результата, а
 * ownership успешного document остаётся только у `document`.
 */
json_status_t json_document_load_file(const char *path, json_document_t *document,
    char *error, size_t error_capacity)
{
    FILE *stream;
    char *text;
    long file_size;
    size_t bytes_read;
    json_status_t status;

    if ((path == NULL) || (document == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    stream = fopen(path, "rb");
    if (stream == NULL) {
        (void)json_set_error(error, error_capacity, "cannot open JSON file");
        return JSON_STATUS_IO_ERROR;
    }

    if ((fseek(stream, 0L, SEEK_END) != 0) || ((file_size = ftell(stream)) < 0L) ||
        (fseek(stream, 0L, SEEK_SET) != 0)) {
        (void)fclose(stream);
        (void)json_set_error(error, error_capacity,
            "cannot determine JSON file size");
        return JSON_STATUS_IO_ERROR;
    }

    if ((unsigned long)file_size >= SIZE_MAX) {
        (void)fclose(stream);
        return JSON_STATUS_CAPACITY_ERROR;
    }

    text = calloc((size_t)file_size + 1U, 1U);
    if (text == NULL) {
        (void)fclose(stream);
        return JSON_STATUS_ALLOCATION_ERROR;
    }

    bytes_read = fread(text, 1U, (size_t)file_size, stream);
    if ((fclose(stream) != 0) || (bytes_read != (size_t)file_size)) {
        free(text);
        (void)json_set_error(error, error_capacity, "cannot read complete JSON file");
        return JSON_STATUS_IO_ERROR;
    }

    status = json_document_parse_text(text, document, error, error_capacity);
    free(text);
    return status;
}

/**
 * @brief Проверяет type token и возвращает typed boolean predicate.
 * @details
 * Алгоритм валидирует token через общий read-only accessor и сравнивает одно enum
 * поле. Несовпадение типа — нормальный результат predicate, не ошибка API.
 */
json_status_t json_token_has_type(const json_document_t *document,
    size_t token_index, json_token_type_t expected_type, json_boolean_t *result)
{
    const json_token_t *token;
    json_status_t status;

    if (result == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, token_index, &token);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    *result = (token->type == expected_type) ? JSON_BOOLEAN_TRUE : JSON_BOOLEAN_FALSE;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Сравнивает raw bytes token с ожидаемой NUL-terminated строкой.
 * @details
 * Алгоритм получает token, сравнивает длину диапазона и затем выполняет побайтный
 * `memcmp()`. Он не декодирует JSON escapes и поэтому сохраняет понятный raw
 * contract для object keys и diagnostic usage.
 */
json_status_t json_token_equals(const json_document_t *document, size_t token_index,
    const char *expected_text, json_boolean_t *result)
{
    const json_token_t *token;
    size_t expected_length;
    json_status_t status;

    if ((expected_text == NULL) || (result == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, token_index, &token);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    expected_length = strlen(expected_text);
    *result = (((token->end - token->start) == expected_length) &&
        (memcmp(document->text + token->start, expected_text, expected_length) == 0))
        ? JSON_BOOLEAN_TRUE : JSON_BOOLEAN_FALSE;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Находит value token непосредственного object field по raw key text.
 * @details
 * Алгоритм требует object token, затем перебирает его direct children парами.
 * После сравнения key helper выдаёт следующий direct child как value; subtree
 * этого value пропускается через `json_subtree_end()` перед следующей парой.
 */
json_status_t json_object_get(const json_document_t *document, size_t object_index,
    const char *key, size_t *value_index)
{
    const json_token_t *object_token;
    size_t cursor;
    json_status_t status;

    if ((key == NULL) || (value_index == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, object_index, &object_token);
    if ((status != JSON_STATUS_SUCCESS) ||
        (object_token->type != JSON_TOKEN_OBJECT)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    cursor = object_index + 1U;
    while ((cursor < document->token_count) &&
        (document->tokens[cursor].parent_index == (ptrdiff_t)object_index)) {
        json_boolean_t key_matches;
        size_t next_cursor;
        size_t candidate_value = cursor + 1U;

        if ((document->tokens[cursor].type != JSON_TOKEN_STRING) ||
            (candidate_value >= document->token_count) ||
            (document->tokens[candidate_value].parent_index !=
                (ptrdiff_t)object_index)) {
            return JSON_STATUS_SYNTAX_ERROR;
        }

        status = json_token_equals(document, cursor, key, &key_matches);
        if (status != JSON_STATUS_SUCCESS) {
            return status;
        }

        if (key_matches == JSON_BOOLEAN_TRUE) {
            *value_index = candidate_value;
            return JSON_STATUS_SUCCESS;
        }

        status = json_subtree_end(document, candidate_value, &next_cursor);
        if (status != JSON_STATUS_SUCCESS) {
            return status;
        }
        cursor = next_cursor;
    }

    return JSON_STATUS_NOT_FOUND;
}

/**
 * @brief Выдаёт число непосредственных элементов array token.
 * @details
 * Алгоритм валидирует document/index/type и копирует `child_count`, который был
 * накоплен parser-ом только для direct array children.
 */
json_status_t json_array_size(const json_document_t *document, size_t array_index,
    size_t *element_count)
{
    const json_token_t *array_token;
    json_status_t status;

    if (element_count == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, array_index, &array_token);
    if ((status != JSON_STATUS_SUCCESS) ||
        (array_token->type != JSON_TOKEN_ARRAY)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    *element_count = array_token->child_count;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Выдаёт token index непосредственного array element по нулевой позиции.
 * @details
 * Алгоритм требует array token и проходит direct children, пропуская каждый
 * найденный subtree через `json_subtree_end()`. Счётчик увеличивается только для
 * прямых элементов, поэтому вложенные токены не искажают element_position.
 */
json_status_t json_array_get(const json_document_t *document, size_t array_index,
    size_t element_position, size_t *value_index)
{
    const json_token_t *array_token;
    size_t cursor;
    size_t current_position = 0U;
    json_status_t status;

    if (value_index == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, array_index, &array_token);
    if ((status != JSON_STATUS_SUCCESS) ||
        (array_token->type != JSON_TOKEN_ARRAY)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    cursor = array_index + 1U;
    while ((cursor < document->token_count) &&
        (document->tokens[cursor].parent_index == (ptrdiff_t)array_index)) {
        size_t next_cursor;

        if (current_position == element_position) {
            *value_index = cursor;
            return JSON_STATUS_SUCCESS;
        }

        status = json_subtree_end(document, cursor, &next_cursor);
        if (status != JSON_STATUS_SUCCESS) {
            return status;
        }

        cursor = next_cursor;
        current_position += 1U;
    }

    return JSON_STATUS_NOT_FOUND;
}

/**
 * @brief Копирует raw token text в allocation, возвращаемую caller-у.
 * @details
 * Алгоритм валидирует token, проверяет переполнение `length + 1`, выделяет строку
 * и копирует точный source range. Output-указатель записывается только после
 * успешной allocation.
 */
json_status_t json_token_copy_text(const json_document_t *document,
    size_t token_index, char **text)
{
    const json_token_t *token;
    char *copy;
    size_t length;
    json_status_t status;

    if (text == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, token_index, &token);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    length = token->end - token->start;
    if (length == SIZE_MAX) {
        return JSON_STATUS_CAPACITY_ERROR;
    }

    copy = malloc(length + 1U);
    if (copy == NULL) {
        return JSON_STATUS_ALLOCATION_ERROR;
    }

    memcpy(copy, document->text + token->start, length);
    copy[length] = '\0';
    *text = copy;
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Освобождает memory allocation, полученный от json-lib.
 * @details
 * Алгоритм передаёт указатель стандартному `free()`, допускающему NULL, и всегда
 * возвращает success status. Helper является публичной status-only заменой
 * обычному void-return освобождению raw token copy.
 */
json_status_t json_memory_free(void *memory)
{
    free(memory);
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Преобразует JSON unsigned integer primitive в `uint64_t`.
 * @details
 * Алгоритм проверяет primitive token, копирует raw text, явно запрещает leading
 * minus и использует `strtoull()` с основанием десять. Успех требует полного
 * потребления строки, отсутствия errno и представимости в `uint64_t`.
 */
json_status_t json_token_to_u64(const json_document_t *document, size_t token_index,
    uint64_t *value)
{
    const json_token_t *token;
    char *text;
    char *end_pointer;
    unsigned long long parsed;
    json_status_t status;

    if (value == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, token_index, &token);
    if ((status != JSON_STATUS_SUCCESS) ||
        (token->type != JSON_TOKEN_PRIMITIVE)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_token_copy_text(document, token_index, &text);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    errno = 0;
    parsed = strtoull(text, &end_pointer, 10);
    if ((text[0] == '-') || (errno != 0) || (end_pointer == text) ||
        (*end_pointer != '\0') || (parsed > UINT64_MAX)) {
        free(text);
        return JSON_STATUS_SYNTAX_ERROR;
    }

    *value = (uint64_t)parsed;
    free(text);
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Преобразует JSON number primitive в конечное `double`.
 * @details
 * Алгоритм требует primitive token, копирует text и использует `strtod()`. Он
 * принимает только полное преобразование без errno и с конечным результатом,
 * поэтому overflow и нечисловые primitives не покидают API как values.
 */
json_status_t json_token_to_double(const json_document_t *document,
    size_t token_index, double *value)
{
    const json_token_t *token;
    char *text;
    char *end_pointer;
    double parsed;
    json_status_t status;

    if (value == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, token_index, &token);
    if ((status != JSON_STATUS_SUCCESS) ||
        (token->type != JSON_TOKEN_PRIMITIVE)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_token_copy_text(document, token_index, &text);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    errno = 0;
    parsed = strtod(text, &end_pointer);
    if ((errno != 0) || (end_pointer == text) || (*end_pointer != '\0') ||
        !isfinite(parsed)) {
        free(text);
        return JSON_STATUS_SYNTAX_ERROR;
    }

    *value = parsed;
    free(text);
    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Преобразует exact JSON boolean primitive в `json_boolean_t`.
 * @details
 * Алгоритм требует primitive token и проверяет raw text сначала против `true`,
 * затем против `false`. Другие primitives, включая `null` и number, возвращают
 * syntax error вместо неявного integer conversion.
 */
json_status_t json_token_to_boolean(const json_document_t *document,
    size_t token_index, json_boolean_t *value)
{
    const json_token_t *token;
    json_boolean_t matches;
    json_status_t status;

    if (value == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_get_token(document, token_index, &token);
    if ((status != JSON_STATUS_SUCCESS) ||
        (token->type != JSON_TOKEN_PRIMITIVE)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_token_equals(document, token_index, "true", &matches);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }
    if (matches == JSON_BOOLEAN_TRUE) {
        *value = JSON_BOOLEAN_TRUE;
        return JSON_STATUS_SUCCESS;
    }

    status = json_token_equals(document, token_index, "false", &matches);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }
    if (matches == JSON_BOOLEAN_TRUE) {
        *value = JSON_BOOLEAN_FALSE;
        return JSON_STATUS_SUCCESS;
    }

    return JSON_STATUS_SYNTAX_ERROR;
}

/**
 * @brief Создаёт temporary stream рядом с целевым JSON-artifact.
 * @details
 * Алгоритм требует пустой writer, копирует target path, безопасно формирует
 * mkstemp template и открывает descriptor как binary writable stream. Любая
 * ошибка после первой allocation очищает transaction через json_writer_abort().
 */
json_status_t json_writer_open(const char *target_path, json_writer_t *writer,
    char *error, size_t error_capacity)
{
    static const char suffix[] = ".tmp.XXXXXX";
    int file_descriptor;
    size_t target_length;
    json_status_t status;

    if ((target_path == NULL) || (writer == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }
    if ((writer->stream != NULL) || (writer->temporary_path != NULL) ||
        (writer->target_path != NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    status = json_duplicate_string(target_path, &writer->target_path);
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    target_length = strlen(target_path);
    if (target_length > (SIZE_MAX - sizeof(suffix))) {
        (void)json_writer_abort(writer);
        return JSON_STATUS_CAPACITY_ERROR;
    }

    writer->temporary_path = malloc(target_length + sizeof(suffix));
    if (writer->temporary_path == NULL) {
        (void)json_writer_abort(writer);
        return JSON_STATUS_ALLOCATION_ERROR;
    }

    memcpy(writer->temporary_path, target_path, target_length);
    memcpy(writer->temporary_path + target_length, suffix, sizeof(suffix));

    file_descriptor = mkstemp(writer->temporary_path);
    if (file_descriptor < 0) {
        (void)json_set_error(error, error_capacity,
            "cannot create temporary JSON file");
        (void)json_writer_abort(writer);
        return JSON_STATUS_IO_ERROR;
    }

    writer->stream = fdopen(file_descriptor, "wb");
    if (writer->stream == NULL) {
        (void)close(file_descriptor);
        (void)json_set_error(error, error_capacity,
            "cannot open temporary JSON stream");
        (void)json_writer_abort(writer);
        return JSON_STATUS_IO_ERROR;
    }

    return JSON_STATUS_SUCCESS;
}

/**
 * @brief Записывает trusted raw JSON syntax fragment в открытый writer stream.
 * @details
 * Алгоритм проверяет transaction state и передаёт фрагмент `fputs()`. Никакого
 * grammar validation здесь нет намеренно: public contract выделяет raw trusted
 * punctuation и безопасное string encoding в разные операции.
 */
json_status_t json_writer_write_raw(json_writer_t *writer, const char *text)
{
    if ((writer == NULL) || (writer->stream == NULL) || (text == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    return (fputs(text, writer->stream) == EOF) ? JSON_STATUS_IO_ERROR :
        JSON_STATUS_SUCCESS;
}

/**
 * @brief Кодирует одну строку как JSON string и записывает её в writer stream.
 * @details
 * Алгоритм открывает значение кавычкой, последовательно кодирует каждый byte и
 * завершает его кавычкой. Кавычки и обратная косая черта получают short escape,
 * control bytes получают fixed-width `\\u00XX`, остальные bytes копируются без
 * Unicode transformation.
 */
json_status_t json_writer_write_string(json_writer_t *writer, const char *text)
{
    const unsigned char *cursor;
    json_status_t status;

    if (writer == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    cursor = (const unsigned char *)((text == NULL) ? "" : text);
    status = json_writer_write_raw(writer, "\"");
    if (status != JSON_STATUS_SUCCESS) {
        return status;
    }

    while (*cursor != '\0') {
        char encoded[7];

        if ((*cursor == '"') || (*cursor == '\\')) {
            encoded[0] = '\\';
            encoded[1] = (char)*cursor;
            encoded[2] = '\0';
        } else if (*cursor < 0x20U) {
            (void)snprintf(encoded, sizeof(encoded), "\\u%04x", *cursor);
        } else {
            encoded[0] = (char)*cursor;
            encoded[1] = '\0';
        }

        status = json_writer_write_raw(writer, encoded);
        if (status != JSON_STATUS_SUCCESS) {
            return status;
        }

        cursor += 1U;
    }

    return json_writer_write_raw(writer, "\"");
}

/**
 * @brief Прерывает writer transaction и очищает все owned resources.
 * @details
 * Алгоритм best-effort закрывает stream, удаляет temporary path, освобождает оба
 * пути и всегда обнуляет writer. Ошибки close/unlink не препятствуют cleanup, но
 * отражаются в итоговом `JSON_STATUS_IO_ERROR` после восстановления инварианта.
 */
json_status_t json_writer_abort(json_writer_t *writer)
{
    json_status_t status = JSON_STATUS_SUCCESS;

    if (writer == NULL) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    if ((writer->stream != NULL) && (fclose(writer->stream) != 0)) {
        status = JSON_STATUS_IO_ERROR;
    }
    if ((writer->temporary_path != NULL) && (unlink(writer->temporary_path) != 0) &&
        (errno != ENOENT)) {
        status = JSON_STATUS_IO_ERROR;
    }

    free(writer->temporary_path);
    free(writer->target_path);
    *writer = (json_writer_t){0};
    return status;
}

/**
 * @brief Синхронизирует temporary stream и публикует JSON-file через rename.
 * @details
 * Алгоритм требует полного transaction state, вызывает `fflush()`, `fsync()` и
 * `fclose()`, после чего atomically заменяет target через `rename()`. Любая
 * неудача записывает diagnostic, запускает cleanup и возвращает I/O status.
 */
json_status_t json_writer_commit(json_writer_t *writer, char *error,
    size_t error_capacity)
{
    if ((writer == NULL) || (writer->stream == NULL) ||
        (writer->temporary_path == NULL) || (writer->target_path == NULL)) {
        return JSON_STATUS_ARGUMENT_ERROR;
    }

    if ((fflush(writer->stream) != 0) || (fsync(fileno(writer->stream)) != 0) ||
        (fclose(writer->stream) != 0)) {
        writer->stream = NULL;
        (void)json_set_error(error, error_capacity,
            "cannot flush and synchronize temporary JSON file");
        (void)json_writer_abort(writer);
        return JSON_STATUS_IO_ERROR;
    }
    writer->stream = NULL;

    if (rename(writer->temporary_path, writer->target_path) != 0) {
        (void)json_set_error(error, error_capacity,
            "cannot atomically publish JSON file");
        (void)json_writer_abort(writer);
        return JSON_STATUS_IO_ERROR;
    }

    free(writer->temporary_path);
    free(writer->target_path);
    *writer = (json_writer_t){0};
    return JSON_STATUS_SUCCESS;
}

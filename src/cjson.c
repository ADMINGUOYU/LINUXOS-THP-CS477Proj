// file: cjson.c

# include "cjson.h"

// include C standard library headers
# include <string.h> // we use this to do O(n) strcmp (for simplicity, no hashing or bst)
# include <stdlib.h> // we use this to do memory allocation
# include <stddef.h> // we use this to do size_t type
# include <errno.h>
# include <limits.h>
# include <float.h>
# include <math.h>
# include <locale.h>
# include <stdio.h>



/* Internal APIs (forward declarations) */
// output cursor type used by the serialisation helpers
struct cjson_output;
// allocation and string ownership helpers
static struct cjson *_new_node(enum cjson_t type);
static char *_copy_string(const char *text);
// whitespace, Unicode and scalar parsing helpers
static void _skip_space(const char **src);
static size_t _utf8_length(const unsigned char *text);
static int _read_hex(const char **src, unsigned int *value);
static char *_parse_string(const char **src);
static struct cjson *_parse_number(const char **src);
// ordered object lookup helper
static size_t _object_index(const struct cjson *json, const char *key);
// output sizing and recursive emission helpers
static int _emit(struct cjson_output *output, const char *text, size_t length);
static int _emit_string(struct cjson_output *output, const char *text);
static int _emit_node(const struct cjson *json, struct cjson_output *output, size_t depth);

// json parsers (recursive), public cjson_parse will call the general internal parser
static struct cjson * _cjson_parse(const char ** src);
static struct cjson * _cjson_parse_object(const char ** src);
static struct cjson * _cjson_parse_array(const char ** src);
static struct cjson * _cjson_parse_value(const char ** src);  // includes string, number, boolean, null
// cjson free helpers (recursive)
static void _cjson_free(struct cjson * json);
// cjson serialise helpers (recursive)
static int _cjson_serialise(const struct cjson * json, char * buffer, size_t buffer_size);



/* Internal API implementations */
/* A bounded output cursor supports a sizing pass before writing caller memory. */
struct cjson_output
{
    char *buffer;
    size_t size;
};

/* Allocate a zero-initialised node, also representing JSON null directly. */
static struct cjson *_new_node(enum cjson_t type)
{
    struct cjson *node = calloc(1, sizeof(*node));

    if (node != NULL)
        node->type = type;
    else
        errno = ENOMEM;
    return node;
}

/* Copy owned strings using C99 facilities rather than POSIX strdup. */
static char *_copy_string(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL)
    {
        errno = EINVAL;
        return NULL;
    }
    length = strlen(text);
    copy = malloc(length + 1);
    if (copy != NULL)
        memcpy(copy, text, length + 1);
    else
        errno = ENOMEM;
    return copy;
}

/* Accept exactly the four whitespace characters allowed by JSON. */
static void _skip_space(const char **src)
{
    while (**src == ' ' || **src == '\t' || **src == '\r' || **src == '\n')
        ++*src;
}

/* Validate a raw UTF-8 sequence, rejecting overlong encodings and surrogates. */
static size_t _utf8_length(const unsigned char *text)
{
    unsigned int value;
    unsigned int minimum;
    size_t length;
    size_t index;

    if (text[0] < 0x80)
        return text[0] >= 0x20 ? 1 : 0;
    if (text[0] >= 0xc2 && text[0] <= 0xdf)
    {
        length = 2;
        value = text[0] & 0x1f;
        minimum = 0x80;
    }
    else if (text[0] >= 0xe0 && text[0] <= 0xef)
    {
        length = 3;
        value = text[0] & 0x0f;
        minimum = 0x800;
    }
    else if (text[0] >= 0xf0 && text[0] <= 0xf4)
    {
        length = 4;
        value = text[0] & 0x07;
        minimum = 0x10000;
    }
    else
        return 0;
    for (index = 1; index < length; ++index)
    {
        if ((text[index] & 0xc0) != 0x80)
            return 0;
        value = (value << 6) | (text[index] & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return 0;
    return length;
}

/* Decode four hexadecimal digits without reading beyond a truncated string. */
static int _read_hex(const char **src, unsigned int *value)
{
    size_t index;
    unsigned int digit;
    char character;

    *value = 0;
    for (index = 0; index < 4; ++index)
    {
        character = **src;
        if (character >= '0' && character <= '9')
            digit = (unsigned int)(character - '0');
        else if (character >= 'a' && character <= 'f')
            digit = (unsigned int)(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F')
            digit = (unsigned int)(character - 'A' + 10);
        else
            return -1;
        *value = *value * 16 + digit;
        ++*src;
    }
    return 0;
}

/* Decode a quoted string; decoded UTF-8 never exceeds its source spelling. */
static char *_parse_string(const char **src)
{
    const char *start;
    const char *cursor;
    char *result;
    size_t capacity;
    size_t count = 0;
    size_t length;
    unsigned int value;
    unsigned int low;
    char character;

    if (**src != '"')
    {
        errno = EINVAL;
        return NULL;
    }
    start = *src + 1;
    cursor = start;
    /* Locate the closing quote, skipping escaped characters for allocation sizing. */
    while (*cursor != '\0' && *cursor != '"')
    {
        if (*cursor == '\\')
        {
            ++cursor;
            if (*cursor == '\0')
                break;
        }
        ++cursor;
    }
    if (*cursor != '"')
    {
        errno = EINVAL;
        return NULL;
    }
    capacity = (size_t)(cursor - start) + 1;
    result = malloc(capacity);
    if (result == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }
    cursor = start;
    while (*cursor != '"')
    {
        character = *cursor;
        ++cursor;
        if (character != '\\')
        {
            length = _utf8_length((const unsigned char *)(cursor - 1));
            if (length == 0)
                goto invalid;
            memcpy(result + count, cursor - 1, length);
            count += length;
            cursor += length - 1;
            continue;
        }
        character = *cursor;
        ++cursor;
        switch (character)
        {
            case '"': result[count] = '"'; ++count; break;
            case '\\': result[count] = '\\'; ++count; break;
            case '/': result[count] = '/'; ++count; break;
            case 'b': result[count] = '\b'; ++count; break;
            case 'f': result[count] = '\f'; ++count; break;
            case 'n': result[count] = '\n'; ++count; break;
            case 'r': result[count] = '\r'; ++count; break;
            case 't': result[count] = '\t'; ++count; break;
            case 'u':
                if (_read_hex(&cursor, &value) != 0 || value == 0)
                    goto invalid;
                /* Join a surrogate pair before encoding its scalar value as UTF-8. */
                if (value >= 0xd800 && value <= 0xdbff)
                {
                    if (*cursor != '\\' || cursor[1] != 'u')
                        goto invalid;
                    cursor += 2;
                    if (_read_hex(&cursor, &low) != 0 || low < 0xdc00 || low > 0xdfff)
                        goto invalid;
                    value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
                }
                else if (value >= 0xdc00 && value <= 0xdfff)
                    goto invalid;
                if (value < 0x80)
                {
                    result[count] = (char)value;
                    ++count;
                }
                else
                {
                    if (value < 0x800)
                    {
                        result[count] = (char)(0xc0 | (value >> 6));
                        ++count;
                    }
                    else
                    {
                        if (value >= 0x10000)
                        {
                            result[count] = (char)(0xf0 | (value >> 18));
                            ++count;
                            result[count] = (char)(0x80 | ((value >> 12) & 0x3f));
                            ++count;
                        }
                        else
                        {
                            result[count] = (char)(0xe0 | (value >> 12));
                            ++count;
                        }
                        result[count] = (char)(0x80 | ((value >> 6) & 0x3f));
                        ++count;
                    }
                    result[count] = (char)(0x80 | (value & 0x3f));
                    ++count;
                }
                break;
            default: goto invalid;
        }
    }
    result[count] = '\0';
    *src = cursor + 1;
    return result;

invalid:
    free(result);
    errno = EINVAL;
    return NULL;
}

/* Look up keys in insertion order using one strcmp per key. */
static size_t _object_index(const struct cjson *json, const char *key)
{
    size_t index;

    for (index = 0; index < json->value.object.size; ++index)
        if (strcmp(json->value.object.keys[index], key) == 0)
            return index;
    return json->value.object.size;
}

/* Parse containers through the existing recursive entry points. */
static struct cjson *_cjson_parse(const char **src)
{
    _skip_space(src);
    if (**src == '{')
        return _cjson_parse_object(src);
    if (**src == '[')
        return _cjson_parse_array(src);
    return _cjson_parse_value(src);
}

/* Parse ordered key/value pairs, transferring ownership only after insertion. */
static struct cjson *_cjson_parse_object(const char **src)
{
    struct cjson *object = cjson_create_object();
    struct cjson *value;
    char *key;
    int failure;

    if (object == NULL)
        return NULL;
    ++*src;
    _skip_space(src);
    if (**src == '}')
    {
        ++*src;
        return object;
    }
    for (;;)
    {
        key = _parse_string(src);
        if (key == NULL)
            goto fail;
        _skip_space(src);
        if (**src != ':')
        {
            free(key);
            errno = EINVAL;
            goto fail;
        }
        ++*src;
        value = _cjson_parse(src);
        if (value == NULL)
        {
            free(key);
            goto fail;
        }
        if (cjson_object_insert(object, key, value) != 0)
        {
            failure = errno;
            free(key);
            cjson_free(value);
            errno = failure;
            goto fail;
        }
        free(key);
        _skip_space(src);
        if (**src == '}')
        {
            ++*src;
            return object;
        }
        if (**src != ',')
        {
            errno = EINVAL;
            goto fail;
        }
        ++*src;
        _skip_space(src);
    }

fail:
    failure = errno;
    cjson_free(object);
    errno = failure;
    return NULL;
}

/* Parse an ordered, heterogeneous array; trailing commas are rejected. */
static struct cjson *_cjson_parse_array(const char **src)
{
    struct cjson *array = cjson_create_array();
    struct cjson *item;
    int failure;

    if (array == NULL)
        return NULL;
    ++*src;
    _skip_space(src);
    if (**src == ']')
    {
        ++*src;
        return array;
    }
    for (;;)
    {
        item = _cjson_parse(src);
        if (item == NULL)
            goto fail;
        if (cjson_array_insert(array, array->value.array.size, item) != 0)
        {
            failure = errno;
            cjson_free(item);
            errno = failure;
            goto fail;
        }
        _skip_space(src);
        if (**src == ']')
        {
            ++*src;
            return array;
        }
        if (**src != ',')
        {
            errno = EINVAL;
            goto fail;
        }
        ++*src;
    }

fail:
    failure = errno;
    cjson_free(array);
    errno = failure;
    return NULL;
}

/* Strictly recognise JSON number syntax before calling C library conversion. */
static struct cjson *_parse_number(const char **src)
{
    const char *cursor = *src;
    char *end;
    long long integer;
    double floating;
    int is_integer = 1;

    if (*cursor == '-')
        ++cursor;
    if (*cursor == '0')
        ++cursor;
    else
    {
        if (*cursor < '1' || *cursor > '9')
            goto invalid;
        while (*cursor >= '0' && *cursor <= '9')
            ++cursor;
    }
    if (*cursor == '.')
    {
        is_integer = 0;
        ++cursor;
        if (*cursor < '0' || *cursor > '9')
            goto invalid;
        while (*cursor >= '0' && *cursor <= '9')
            ++cursor;
    }
    if (*cursor == 'e' || *cursor == 'E')
    {
        is_integer = 0;
        ++cursor;
        if (*cursor == '+' || *cursor == '-')
            ++cursor;
        if (*cursor < '0' || *cursor > '9')
            goto invalid;
        while (*cursor >= '0' && *cursor <= '9')
            ++cursor;
    }
    /* Integer literals preserve all long-long bits instead of rounding via double. */
    errno = 0;
    if (is_integer)
    {
        integer = strtoll(*src, &end, 10);
        if (errno == ERANGE)
            return NULL;
        if (end != cursor)
            goto invalid;
        *src = cursor;
        return cjson_create_integer(integer);
    }
    if (strcmp(localeconv()->decimal_point, ".") != 0)
        goto invalid;
    floating = strtod(*src, &end);
    if (errno == ERANGE || !isfinite(floating))
    {
        errno = ERANGE;
        return NULL;
    }
    if (end != cursor)
        goto invalid;
    *src = cursor;
    return cjson_create_floating(floating);

invalid:
    errno = EINVAL;
    return NULL;
}

/* Decode one scalar; enclosing parsers enforce its terminating delimiter. */
static struct cjson *_cjson_parse_value(const char **src)
{
    struct cjson *node;
    char *text;

    if (**src == '"')
    {
        text = _parse_string(src);
        if (text == NULL)
            return NULL;
        node = _new_node(CJSON_STRING);
        if (node == NULL)
            free(text);
        else
            node->value.string = text;
        return node;
    }
    if (strncmp(*src, "true", 4) == 0)
    {
        *src += 4;
        return cjson_create_boolean(1);
    }
    if (strncmp(*src, "false", 5) == 0)
    {
        *src += 5;
        return cjson_create_boolean(0);
    }
    if (strncmp(*src, "null", 4) == 0)
    {
        *src += 4;
        return _new_node(CJSON_NULL);
    }
    return _parse_number(src);
}

/* Free children before their containing arrays; NULL denotes no owned resource. */
static void _cjson_free(struct cjson *json)
{
    size_t index;

    if (json == NULL)
        return;
    if (json->type == CJSON_STRING)
        free(json->value.string);
    else if (json->type == CJSON_ARRAY)
    {
        for (index = 0; index < json->value.array.size; ++index)
            _cjson_free(json->value.array.items[index]);
        free(json->value.array.items);
    }
    else if (json->type == CJSON_OBJECT)
    {
        for (index = 0; index < json->value.object.size; ++index)
        {
            free(json->value.object.keys[index]);
            _cjson_free(json->value.object.values[index]);
        }
        free(json->value.object.keys);
        free(json->value.object.values);
    }
    free(json);
}

/* Append bytes during sizing or writing, reserving room for the final NUL. */
static int _emit(struct cjson_output *output, const char *text, size_t length)
{
    if (length > (size_t)-1 - output->size - 1)
    {
        errno = EOVERFLOW;
        return -1;
    }
    if (output->buffer != NULL)
        memcpy(output->buffer + output->size, text, length);
    output->size += length;
    return 0;
}

/* Escape a string while validating raw UTF-8 supplied by callers. */
static int _emit_string(struct cjson_output *output, const char *text)
{
    char escaped[7];
    size_t length;
    unsigned char character;

    if (text == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (_emit(output, "\"", 1) != 0)
        return -1;
    while (*text != '\0')
    {
        character = (unsigned char)*text;
        if (character == '"' || character == '\\')
        {
            escaped[0] = '\\';
            escaped[1] = (char)character;
            if (_emit(output, escaped, 2) != 0)
                return -1;
            ++text;
        }
        else if (character < 0x20)
        {
            snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int)character);
            if (_emit(output, escaped, 6) != 0)
                return -1;
            ++text;
        }
        else
        {
            length = _utf8_length((const unsigned char *)text);
            if (length == 0)
            {
                errno = EINVAL;
                return -1;
            }
            if (_emit(output, text, length) != 0)
                return -1;
            text += length;
        }
    }
    return _emit(output, "\"", 1);
}

/* Recursively emit compact JSON, retaining floating subtype with a decimal suffix. */
static int _emit_node(const struct cjson *json, struct cjson_output *output, size_t depth)
{
    char number[128];
    int format_result;
    size_t length;
    size_t index;

    if (json == NULL || depth > 256 ||
        (depth == 256 && (json->type == CJSON_ARRAY || json->type == CJSON_OBJECT)))
    {
        errno = EINVAL;
        return -1;
    }
    switch (json->type)
    {
        case CJSON_NULL: return _emit(output, "null", 4);
        case CJSON_BOOL: return _emit(output, json->value.boolean ? "true" : "false", json->value.boolean ? 4 : 5);
        case CJSON_STRING: return _emit_string(output, json->value.string);
        case CJSON_NUMBER:
            if (json->value.number.is_integer)
                format_result = snprintf(number, sizeof(number), "%lld", json->value.number.number.integer);
            else
            {
                if (!isfinite(json->value.number.number.floating) || strcmp(localeconv()->decimal_point, ".") != 0)
                {
                    errno = EINVAL;
                    return -1;
                }
                /* C99 DECIMAL_DIG supplies sufficient digits for a double round trip. */
                format_result = snprintf(number, sizeof(number), "%.*g", DECIMAL_DIG, json->value.number.number.floating);
            }
            /* snprintf returns a signed status; validate before using size_t lengths. */
            if (format_result < 0 || (size_t)format_result >= sizeof(number))
            {
                errno = EOVERFLOW;
                return -1;
            }
            length = (size_t)format_result;
            if (!json->value.number.is_integer &&
                strchr(number, '.') == NULL && strchr(number, 'e') == NULL && strchr(number, 'E') == NULL)
            {
                if (length + 2 >= sizeof(number))
                {
                    errno = EOVERFLOW;
                    return -1;
                }
                memcpy(number + length, ".0", 3);
                length += 2;
            }
            return _emit(output, number, length);
        case CJSON_ARRAY:
            if (json->value.array.size != 0 && json->value.array.items == NULL)
                break;
            if (_emit(output, "[", 1) != 0)
                return -1;
            for (index = 0; index < json->value.array.size; ++index)
            {
                if (index != 0 && _emit(output, ",", 1) != 0)
                    return -1;
                if (_emit_node(json->value.array.items[index], output, depth + 1) != 0)
                    return -1;
            }
            return _emit(output, "]", 1);
        case CJSON_OBJECT:
            if (json->value.object.size != 0 && (json->value.object.keys == NULL || json->value.object.values == NULL))
                break;
            if (_emit(output, "{", 1) != 0)
                return -1;
            for (index = 0; index < json->value.object.size; ++index)
            {
                if (index != 0 && _emit(output, ",", 1) != 0)
                    return -1;
                if (_emit_string(output, json->value.object.keys[index]) != 0 || _emit(output, ":", 1) != 0 ||
                    _emit_node(json->value.object.values[index], output, depth + 1) != 0)
                    return -1;
            }
            return _emit(output, "}", 1);
        default: break;
    }
    errno = EINVAL;
    return -1;
}

/* Size first so an insufficient buffer is never partially overwritten. */
static int _cjson_serialise(const struct cjson *json, char *buffer, size_t buffer_size)
{
    struct cjson_output output = { NULL, 0 };

    if (buffer == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    // dry run without copying anything,
    // just to get the size of the output
    if (_emit_node(json, &output, 0) != 0)
        return -1;
    if (buffer_size < output.size + 1)
    {
        errno = ENOSPC;
        return -1;
    }
    output.buffer = buffer;
    output.size = 0;
    if (_emit_node(json, &output, 0) != 0)
        return -1;
    buffer[output.size] = '\0';
    return 0;
}



/* Public API implementations */
/* Bound recursion in a lexical pre-pass without global state or thread-local storage. */
struct cjson *cjson_parse(const char *src)
{
    const char *cursor;
    const char *scan;
    size_t depth = 0;
    int in_string = 0;
    struct cjson *json;

    if (src == NULL)
    {
        errno = EINVAL;
        return NULL;
    }
    for (scan = src; *scan != '\0'; ++scan)
    {
        if (in_string && *scan == '\\')
        {
            if (scan[1] == '\0')
                break;
            ++scan;
        }
        else if (*scan == '"')
            in_string = !in_string;
        else if (!in_string && (*scan == '[' || *scan == '{'))
        {
            ++depth;
            if (depth > 256)
            {
                errno = EOVERFLOW;
                return NULL;
            }
        }
        else if (!in_string && (*scan == ']' || *scan == '}') && depth != 0)
            --depth;
    }
    cursor = src;
    json = _cjson_parse(&cursor);
    if (json == NULL)
        return NULL;
    _skip_space(&cursor);
    if (*cursor != '\0')
    {
        cjson_free(json);
        errno = EINVAL;
        return NULL;
    }
    return json;
}

/* Serialise through the shared sizing/writing implementation. */
int cjson_serialise(const struct cjson *json, char *buffer, size_t buffer_size)
{
    return _cjson_serialise(json, buffer, buffer_size);
}

/* Release an entire uniquely owned tree. */
void cjson_free(struct cjson *json)
{
    _cjson_free(json);
}

/* A NULL pointer reports the null type but is not an allocated JSON-null node. */
enum cjson_t cjson_get_type(const struct cjson *json)
{
    return json == NULL ? CJSON_NULL : json->type;
}

/* Distinguish stored integer literals from stored floating-point values. */
int cjson_is_integer(const struct cjson *json)
{
    return json != NULL && json->type == CJSON_NUMBER && json->value.number.is_integer != 0;
}

/* Read or update a boolean, normalising nonzero inputs to one. */
int cjson_boolean(const struct cjson *json, int *set_value)
{
    int old_value;

    if (json == NULL || json->type != CJSON_BOOL)
    {
        errno = EINVAL;
        return 0;
    }
    old_value = json->value.boolean;
    if (set_value != NULL)
        ((struct cjson *)json)->value.boolean = *set_value != 0;
    return old_value;
}

/* Require integer subtype so large values are never silently narrowed or rounded. */
long long int cjson_integer(const struct cjson *json, long long int *set_value)
{
    long long old_value;

    if (!cjson_is_integer(json))
    {
        errno = EINVAL;
        return 0;
    }
    old_value = json->value.number.number.integer;
    if (set_value != NULL)
        ((struct cjson *)json)->value.number.number.integer = *set_value;
    return old_value;
}

/* Require floating subtype and reject values that have no JSON representation. */
double cjson_floating(const struct cjson *json, double *set_value)
{
    double old_value;

    if (json == NULL || json->type != CJSON_NUMBER || json->value.number.is_integer ||
        (set_value != NULL && !isfinite(*set_value)))
    {
        errno = EINVAL;
        return 0;
    }
    old_value = json->value.number.number.floating;
    if (set_value != NULL)
        ((struct cjson *)json)->value.number.number.floating = *set_value;
    return old_value;
}

/* Allocate before freeing, so setting from a substring of the old value is safe. */
const char *cjson_string(const struct cjson *json, const char *set_value)
{
    char *copy;

    if (json == NULL || json->type != CJSON_STRING)
    {
        errno = EINVAL;
        return NULL;
    }
    if (set_value != NULL)
    {
        copy = _copy_string(set_value);
        if (copy == NULL)
            return NULL;
        free(json->value.string);
        ((struct cjson *)json)->value.string = copy;
    }
    return json->value.string;
}

/* Return the number of ordered array elements. */
size_t cjson_array_size(const struct cjson *json)
{
    if (json == NULL || json->type != CJSON_ARRAY)
    {
        errno = EINVAL;
        return 0;
    }
    return json->value.array.size;
}

/* Replacement detaches the old child without freeing it. */
struct cjson *cjson_array_item(const struct cjson *json, size_t index, struct cjson *set_value)
{
    struct cjson *old_value;

    if (json == NULL || json->type != CJSON_ARRAY || index >= json->value.array.size || set_value == json)
    {
        errno = EINVAL;
        return NULL;
    }
    old_value = json->value.array.items[index];
    if (set_value != NULL)
        ((struct cjson *)json)->value.array.items[index] = set_value;
    return old_value;
}

/* Grow before shifting elements, leaving caller ownership unchanged on failure. */
int cjson_array_insert(struct cjson *json, size_t index, struct cjson *new_item)
{
    struct cjson **items;
    size_t size;

    if (json == NULL || json->type != CJSON_ARRAY || new_item == NULL || new_item == json || index > json->value.array.size)
    {
        errno = EINVAL;
        return -1;
    }
    size = json->value.array.size;
    if (size >= (size_t)-1 / sizeof(*items))
    {
        errno = EOVERFLOW;
        return -1;
    }
    items = realloc(json->value.array.items, (size + 1) * sizeof(*items));
    if (items == NULL)
    {
        errno = ENOMEM;
        return -1;
    }
    memmove(items + index + 1, items + index, (size - index) * sizeof(*items));
    items[index] = new_item;
    json->value.array.items = items;
    json->value.array.size = size + 1;
    return 0;
}

/* Free the removed element and close the gap without changing relative order. */
int cjson_array_remove(struct cjson *json, size_t index)
{
    size_t size;

    if (json == NULL || json->type != CJSON_ARRAY || index >= json->value.array.size)
    {
        errno = EINVAL;
        return -1;
    }
    size = json->value.array.size;
    cjson_free(json->value.array.items[index]);
    memmove(json->value.array.items + index, json->value.array.items + index + 1,
            (size - index - 1) * sizeof(*json->value.array.items));
    --json->value.array.size;
    return 0;
}

/* Return the number of ordered object members. */
size_t cjson_object_size(const struct cjson *json)
{
    if (json == NULL || json->type != CJSON_OBJECT)
    {
        errno = EINVAL;
        return 0;
    }
    return json->value.object.size;
}

/* Search one key at a time; a replacement retains its position in the object. */
struct cjson *cjson_object_value(const struct cjson *json, const char *key, struct cjson *set_value)
{
    size_t index;
    struct cjson *old_value;

    if (json == NULL || json->type != CJSON_OBJECT || key == NULL || set_value == json)
    {
        errno = EINVAL;
        return NULL;
    }
    index = _object_index(json, key);
    if (index == json->value.object.size)
    {
        errno = ENOENT;
        return NULL;
    }
    old_value = json->value.object.values[index];
    if (set_value != NULL)
        ((struct cjson *)json)->value.object.values[index] = set_value;
    return old_value;
}

/* Allocate both parallel arrays before committing, so allocation failure is atomic. */
int cjson_object_insert(struct cjson *json, const char *key, struct cjson *new_value)
{
    char **keys;
    struct cjson **values;
    char *key_copy;
    size_t size;

    if (json == NULL || json->type != CJSON_OBJECT || key == NULL || new_value == NULL || new_value == json)
    {
        errno = EINVAL;
        return -1;
    }
    size = json->value.object.size;
    if (_object_index(json, key) != size)
    {
        errno = EEXIST;
        return -1;
    }
    if (size >= (size_t)-1 / sizeof(*keys) || size >= (size_t)-1 / sizeof(*values))
    {
        errno = EOVERFLOW;
        return -1;
    }
    key_copy = _copy_string(key);
    if (key_copy == NULL)
        return -1;
    keys = malloc((size + 1) * sizeof(*keys));
    values = malloc((size + 1) * sizeof(*values));
    if (keys == NULL || values == NULL)
    {
        free(keys);
        free(values);
        free(key_copy);
        errno = ENOMEM;
        return -1;
    }
    if (size != 0)
    {
        memcpy(keys, json->value.object.keys, size * sizeof(*keys));
        memcpy(values, json->value.object.values, size * sizeof(*values));
    }
    keys[size] = key_copy;
    values[size] = new_value;
    free(json->value.object.keys);
    free(json->value.object.values);
    json->value.object.keys = keys;
    json->value.object.values = values;
    json->value.object.size = size + 1;
    return 0;
}

/* Remove a matching key and its owned value, preserving the remaining order. */
int cjson_object_remove(struct cjson *json, const char *key)
{
    size_t index;
    size_t size;

    if (json == NULL || json->type != CJSON_OBJECT || key == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    size = json->value.object.size;
    index = _object_index(json, key);
    if (index == size)
    {
        errno = ENOENT;
        return -1;
    }
    free(json->value.object.keys[index]);
    cjson_free(json->value.object.values[index]);
    memmove(json->value.object.keys + index, json->value.object.keys + index + 1,
            (size - index - 1) * sizeof(*json->value.object.keys));
    memmove(json->value.object.values + index, json->value.object.values + index + 1,
            (size - index - 1) * sizeof(*json->value.object.values));
    --json->value.object.size;
    return 0;
}

/* Construct a boolean with its canonical integer representation. */
struct cjson *cjson_create_boolean(int value)
{
    struct cjson *json = _new_node(CJSON_BOOL);

    if (json != NULL)
        json->value.boolean = value != 0;
    return json;
}

/* Construct a full-width signed integer without a floating-point conversion. */
struct cjson *cjson_create_integer(long long int value)
{
    struct cjson *json = _new_node(CJSON_NUMBER);

    if (json != NULL)
    {
        json->value.number.is_integer = 1;
        json->value.number.number.integer = value;
    }
    return json;
}

/* Reject NaN and infinity before allocating a numeric node. */
struct cjson *cjson_create_floating(double value)
{
    struct cjson *json;

    if (!isfinite(value))
    {
        errno = EINVAL;
        return NULL;
    }
    json = _new_node(CJSON_NUMBER);
    if (json != NULL)
        json->value.number.number.floating = value;
    return json;
}

/* Construct an independently owned string copy. */
struct cjson *cjson_create_string(const char *value)
{
    struct cjson *json;
    char *copy = _copy_string(value);

    if (copy == NULL)
        return NULL;
    json = _new_node(CJSON_STRING);
    if (json == NULL)
        free(copy);
    else
        json->value.string = copy;
    return json;
}

/* Construct an empty ordered array. */
struct cjson *cjson_create_array(void)
{
    return _new_node(CJSON_ARRAY);
}

/* Construct an empty ordered object without lookup tables or hashing. */
struct cjson *cjson_create_object(void)
{
    return _new_node(CJSON_OBJECT);
}

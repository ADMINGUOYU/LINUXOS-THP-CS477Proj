// file: commons.c

# include "commons.h"

// include C standard library headers
# include <stdio.h>
# include <string.h>
# include <stdlib.h>
# include <errno.h>
# include <stdint.h>

// include cjson library
# include "cjson.h"


// [Utilities] (logging) =================
void commons_log(const char * module, enum log_level level, const char * message)
{
    // Preserve the caller's error and tolerate missing diagnostic text.
    int saved_errno = errno;

    if (module == NULL)
        module = "commons";
    if (message == NULL)
        message = "";
    // if we use pretty logging
# ifdef logging_fancy
    fprintf(stdout,
        "\033[1;%sm[%s] %s\033[0m: %s\n",
        logging_colours(level),
        module,
        logging_levels(level),
        message);
# else
    fprintf(stdout, "[%s] %s: %s\n", module, logging_levels(level), message);
# endif // logging_fancy
    // flush stdout to ensure the log message is printed immediately
    fflush(stdout);
    errno = saved_errno;
    return;
}

// [SYSTEM CONFIGURATION] ================
// Merge defaults and local overrides, then copy the requested configuration fields.
int load_sys_policy(struct sys_config * policy)
{
    // Policy keys follow the field order in struct sys_config
    const char * const policy_section = "policy";
    const char * const policy_keys[] =
    {
        "base_page_bytes", "region_bytes", "density_threshold_pages",
        "run_threshold_pages", "collapse_retries"
    };
    struct sys_config parsed;
    size_t * fields[] =
    {
        &parsed.base_page_bytes, &parsed.region_bytes, &parsed.density_threshold_pages,
        &parsed.run_threshold_pages, &parsed.collapse_retries
    };
    // An explicit stack merges nested objects without a recursive helper function.
    struct merge_frame
    {
        struct cjson * base;
        struct cjson * overrides;
        size_t index;
    };
    struct merge_frame * stack = NULL;
    struct merge_frame * expanded;
    struct merge_frame * frame;
    size_t depth = 0;
    size_t capacity = 0;
    size_t file_index;
    size_t index;
    size_t path_size;
    size_t file_size = 0;
    long file_position;
    long long value;
    FILE * stream;
    char * path;
    char * buffer;
    const char * key;
    struct cjson * config;
    struct cjson * incoming;
    struct cjson * section;
    struct cjson * node;
    struct cjson * existing;
    int failure = 0;

    if (policy == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    config = cjson_create_object();
    if (config == NULL)
        return -1;

    // Missing files are optional, matching Python; other I/O errors are reported.
    for (file_index = 0; failure == 0 && file_index < sizeof(project_config) / sizeof(project_config[0]); ++file_index)
    {
        if (strlen(project_root) > SIZE_MAX - strlen(project_config[file_index]) - 2)
        {
            failure = EOVERFLOW;
            break;
        }
        path_size = strlen(project_root) + strlen(project_config[file_index]) + 2;
        path = malloc(path_size);
        if (path == NULL)
        {
            failure = ENOMEM;
            break;
        }
        snprintf(path, path_size, "%s/%s", project_root, project_config[file_index]);
        stream = fopen(path, "rb");
        failure = stream == NULL && errno != ENOENT ? errno : 0;
        free(path);
        if (stream == NULL)
            continue;

        // Read a complete, NUL-terminated JSON document, rejecting embedded NULs.
        buffer = NULL;
        if (fseek(stream, 0, SEEK_END) != 0)
            failure = errno;
        if (failure == 0)
        {
            file_position = ftell(stream);
            if (file_position < 0)
                failure = errno;
            else if ((uintmax_t)file_position >= (uintmax_t)SIZE_MAX)
                failure = EOVERFLOW;
            else
                file_size = (size_t)file_position;
        }
        if (failure == 0 && fseek(stream, 0, SEEK_SET) != 0)
            failure = errno;
        if (failure == 0)
        {
            buffer = malloc(file_size + 1);
            if (buffer == NULL)
                failure = ENOMEM;
            else if (fread(buffer, 1, file_size, stream) != file_size)
                failure = EIO;
            else if (memchr(buffer, '\0', file_size) != NULL)
                failure = EINVAL;
            else
                buffer[file_size] = '\0';
        }
        if (fclose(stream) != 0 && failure == 0)
            failure = errno;
        incoming = NULL;
        if (failure == 0)
        {
            incoming = cjson_parse(buffer);
            if (incoming == NULL)
                failure = errno;
            else if (cjson_get_type(incoming) != CJSON_OBJECT)
                failure = EINVAL;
        }
        free(buffer);

        // Descend only when both values are objects; otherwise replace the value.
        // Arrays and explicit nulls replace defaults, just as in Python.
        if (failure == 0 && capacity == 0)
        {
            stack = malloc(sizeof(*stack));
            if (stack == NULL)
                failure = ENOMEM;
            else
                capacity = 1;
        }
        if (failure == 0)
        {
            stack[0] = (struct merge_frame){ config, incoming, 0 };
            depth = 1;
        }
        while (failure == 0 && depth != 0)
        {
            frame = &stack[depth - 1];
            if (frame->index == cjson_object_size(frame->overrides))
            {
                --depth;
                continue;
            }
            index = frame->index;
            ++frame->index;
            key = frame->overrides->value.object.keys[index];
            node = frame->overrides->value.object.values[index];
            existing = cjson_object_value(frame->base, key, NULL);
            if (existing != NULL && cjson_get_type(existing) == CJSON_OBJECT &&
                cjson_get_type(node) == CJSON_OBJECT)
            {
                if (depth == capacity)
                {
                    if (capacity >= SIZE_MAX / sizeof(*stack))
                    {
                        failure = EOVERFLOW;
                        break;
                    }
                    expanded = realloc(stack, (capacity + 1) * sizeof(*stack));
                    if (expanded == NULL)
                    {
                        failure = ENOMEM;
                        break;
                    }
                    stack = expanded;
                    ++capacity;
                }
                stack[depth] = (struct merge_frame){ existing, node, 0 };
                ++depth;
            }
            else
            {
                // Transfer ownership; nulling the source slot prevents double frees.
                if (existing == NULL && cjson_object_insert(frame->base, key, node) != 0)
                    failure = errno;
                else
                {
                    if (existing != NULL)
                        cjson_free(cjson_object_value(frame->base, key, node));
                    frame->overrides->value.object.values[index] = NULL;
                }
            }
        }
        cjson_free(incoming);
    }

    // Keep key paths hard-coded here; validate only the final merged values.
    section = NULL;
    if (failure == 0)
    {
        section = cjson_object_value(config, policy_section, NULL);
        if (section == NULL || cjson_get_type(section) != CJSON_OBJECT)
            failure = EINVAL;
    }
    for (index = 0; failure == 0 && index < sizeof(policy_keys) / sizeof(policy_keys[0]); ++index)
    {
        node = cjson_object_value(section, policy_keys[index], NULL);
        if (!cjson_is_integer(node))
            failure = EINVAL;
        else
        {
            value = cjson_integer(node, NULL);
            if (value < 0 || (uintmax_t)value > (uintmax_t)SIZE_MAX)
                failure = ERANGE;
            else
                *fields[index] = (size_t)value;
        }
    }

    // Release both merged data and traversal storage; publish only on full success
    free(stack);
    cjson_free(config);
    if (failure != 0)
    {
        errno = failure;
        return -1;
    }
    *policy = parsed;
    return 0;
}
// [SYSTEM CONFIGURATION] ENDs ===========

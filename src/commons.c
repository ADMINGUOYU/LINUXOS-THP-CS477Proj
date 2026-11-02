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

// [SYSTEM POLICY] =======================
// Read the local JSON directly and store its policy fields.
int load_sys_policy(struct sys_policy * policy)
{
    // Policy keys follow the field order in struct sys_policy.
    const char * const policy_section = "policy";
    const char * const policy_keys[] = {
        "base_page_bytes", "region_bytes", "density_threshold_pages",
        "run_threshold_pages", "collapse_retries"
    };
    struct sys_policy parsed;
    size_t * fields[] = {
        &parsed.base_page_bytes, &parsed.region_bytes, &parsed.density_threshold_pages,
        &parsed.run_threshold_pages, &parsed.collapse_retries
    };
    FILE * stream = NULL;
    struct cjson * config = NULL;
    struct cjson * section = NULL;
    struct cjson * node;
    char * path = NULL;
    char * buffer = NULL;
    size_t path_size;
    size_t file_size = 0;
    size_t index;
    long file_position;
    long long value;
    int failure = 0;

    // Construct the configured local file path; no default merging is performed.
    if (policy == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (strlen(project_root) > SIZE_MAX - strlen(project_config[1]) - 2)
    {
        errno = EOVERFLOW;
        return -1;
    }
    path_size = strlen(project_root) + strlen(project_config[1]) + 2;
    path = malloc(path_size);
    if (path == NULL)
        return -1;
    snprintf(path, path_size, "%s/%s", project_root, project_config[1]);
    stream = fopen(path, "rb");
    free(path);
    if (stream == NULL)
        return -1;

    // Read the file into a NUL-terminated buffer for cjson_parse.
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

    // Look up each local policy key and copy representable integers into the struct.
    if (failure == 0)
    {
        config = cjson_parse(buffer);
        if (config == NULL)
            failure = errno;
        else
        {
            section = cjson_object_value(config, policy_section, NULL);
            if (section == NULL || cjson_get_type(section) != CJSON_OBJECT)
                failure = EINVAL;
        }
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

    // Release the input and parsed tree on every path; commit only a complete policy.
    cjson_free(config);
    free(buffer);
    if (failure != 0)
    {
        errno = failure;
        return -1;
    }
    *policy = parsed;
    return 0;
}
// [SYSTEM POLICY] ENDs ===================

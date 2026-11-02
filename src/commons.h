// file: commons.h [FULL header]

# ifndef COMMONS_H
# define COMMONS_H

// include standard libraries
# include <stddef.h> // for size_t



// [CONFIGURATION] =======================
# ifndef COMMONS_PROJECT_ROOT
# define COMMONS_PROJECT_ROOT "."
# endif
// Make supplies an absolute root; standalone compilation uses the current directory.
static const char * const project_root = COMMONS_PROJECT_ROOT;
static const char * const project_config[2] = { "config.default.json", "config.local.json" };
# define logging_fancy // comment this or undefine to disable fancy logging
# define logging_colours(level) \
    (level == LOG_LEVEL_ERROR ? "31" : \
    (level == LOG_LEVEL_WARN ? "33" : \
    (level == LOG_LEVEL_INFO ? "34" : \
    (level == LOG_LEVEL_OK ? "32" : \
    (level == LOG_LEVEL_DEBUG ? "36" : "")))))
# define logging_levels(level) \
    (level == LOG_LEVEL_ERROR ? "ERROR" : \
    (level == LOG_LEVEL_WARN ? "WARN" : \
    (level == LOG_LEVEL_INFO ? "INFO" : \
    (level == LOG_LEVEL_OK ? "OK" : \
    (level == LOG_LEVEL_DEBUG ? "DEBUG" : "")))))
// [CONFIGURATION] ENDs ==================


// [Utilities] (logging) =================
// define log levels
enum log_level
{
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3,
    LOG_LEVEL_OK = 4
};
// logging (pritty print)
// Avoid colliding with the standard C log(double) function.
void commons_log(const char * module, enum log_level level, const char * message);
// [Utilities] (logging) ENDs ============


// [SYSTEM POLICY] =======================
// define the system policy struct
struct sys_policy
{
    size_t base_page_bytes; // the size of a base page in bytes
    size_t region_bytes; // the size of a region in bytes
    size_t density_threshold_pages; // the number of pages that must be allocated to consider a region dense
    size_t run_threshold_pages; // the number of pages that must be allocated to consider a region a run
    size_t collapse_retries; // the number of times to retry collapsing a region before giving up
};

// function to read the system policy from a json file
// Read project_root/project_config[1] and copy the five policy keys.
// Return 0 on success, -1 with errno on file/JSON/type errors; output is unchanged.
int load_sys_policy(struct sys_policy * policy);

// [SYSTEM POLICY] ENDs ===================

# endif // COMMONS_H    

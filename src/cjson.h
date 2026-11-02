// file: cjson.h
// used to parse JSON data, stored in struct,
// provides simple interface to access/manipulate JSON data
// C source following C99

# ifndef CJSON_H
# define CJSON_H

// C standard library headers
# include <stddef.h>


// JSON data types (should be publicly accessible)
enum cjson_t
{
    CJSON_NULL = 0U,  // null value
    CJSON_BOOL,
    CJSON_NUMBER,
    CJSON_STRING,
    CJSON_ARRAY,
    CJSON_OBJECT
};

// CJSON data structure
struct cjson
{
    // type of JSON data
    enum cjson_t type;
    // value of JSON data
    union
    {
        int boolean;
        // number of integer type and floating point type
        struct
        {
            union
            {
                long long int integer;
                double floating;
            } number;
            int is_integer;
        } number;
        char *string;
        // arrays (its type can be any of the accepted JSON types)
        // track with cjson objects
        struct
        {
            // items[0] points to the first item
            // should always be ordered
            struct cjson **items;
            size_t size;
        } array;
        struct
        {
            // keys[0] points to the first key
            // values[0] points to the first value
            // keys and values should always be ordered in the same way
            char **keys;
            struct cjson **values;
            size_t size;
        } object;
    } value;
};


/* Public API (I/O) */
// parse JSON data from string/readable stream (buffer)
struct cjson * cjson_parse(const char * src);
// serialise JSON data to string/writable stream (buffer)
// return 0 on success, -1 on failure (i.e. buffer is too small)
int cjson_serialise(const struct cjson * json, char * buffer, size_t buffer_size);
// delete cjson object and free memory
void cjson_free(struct cjson * json);


/* Public API (access/manipulate) */
/* NOTE: always check the type first, we are not responsible here */
/* as this is just a minimalist implementation */
// get type of JSON data (accept a generalised json node)
enum cjson_t cjson_get_type(const struct cjson * json);
// helper for integer and floating point number (check)
int cjson_is_integer(const struct cjson * json);
// get/set value of JSON data (accept a generalised json node)
// this is a series of functions to get/set the correct values
// each function returns the read value, if set_value is not NULL,
// set the value and return the OLD value, if set_value is NULL,
// return the current value
int cjson_boolean(const struct cjson * json, int * set_value);
long long int cjson_integer(const struct cjson * json, long long int * set_value);
double cjson_floating(const struct cjson * json, double * set_value);
// we return const char * to the ORIGINAL string address (no copying)
// but setting a new string value will allocate new memory and free the old one
const char * cjson_string(const struct cjson * json, const char * set_value);
// Exception to the old-value rule: string setters return the NEW stored string,
// because the old allocation is freed. Failure returns NULL and preserves the old value.
// array is bit more complicated, we need a indexer
// helper: get the size of the array
size_t cjson_array_size(const struct cjson * json);
// get or set (no sequence change for insert/remove use the next function)
struct cjson * cjson_array_item(const struct cjson * json, size_t index, struct cjson * set_value);
// insert or remove an item at a specific index
// (index is 0-based (unsigned), to append, use the array size, we do range check for you)
// return 0 on success, -1 on failure (i.e. index out of range)
int cjson_array_insert(struct cjson * json, size_t index, struct cjson * new_item);
int cjson_array_remove(struct cjson * json, size_t index);
// object is similar to array, but we need a key-value pair
size_t cjson_object_size(const struct cjson * json);
// get or set (no sequence change for insert/remove use the next function)
struct cjson * cjson_object_value(const struct cjson * json, const char * key, struct cjson * set_value);
// insert or remove a key-value pair
// (key is a string, we do range check for you)
// return 0 on success, -1 on failure (i.e. key not found)
int cjson_object_insert(struct cjson * json, const char * key, struct cjson * new_value);
int cjson_object_remove(struct cjson * json, const char * key);


/* Public API (utilities) */
// cjson object creators
// (although you can use the cjson_parse function, this is more direct)
struct cjson * cjson_create_boolean(int value);
struct cjson * cjson_create_integer(long long int value);
struct cjson * cjson_create_floating(double value);
struct cjson * cjson_create_string(const char * value);
// for the object lists, you edit them using the (access/manipulate) API above
struct cjson * cjson_create_array(void);
struct cjson * cjson_create_object(void);


/* Internal APIs: static function prototypes in cjson.c */
/* Ownership and errors:
 * Containers own inserted nodes. Supply detached, uniquely owned nodes only;
 * never insert an ancestor, share children, or directly create cycles in structs.
 * Replacements return the detached old node (caller must free it); removals free it.
 * Setting the same node is a no-op: its returned pointer remains container-owned.
 * Keys and strings are copied. Duplicate keys are rejected with errno = EEXIST.
 * Setter APIs retain the original const signatures, but require mutable nodes.
 * Failures use NULL/-1 and errno; scalar getters use zero on invalid types.
 * Serialisation: buffer_size is the buffer capacity in bytes, INCLUDING room
 * for the terminating NUL, passed by value. NULL buffers return -1/EINVAL. Too-small
 * buffers return -1/ENOSPC without modification. Output is compact JSON.
 * Output memory must not overlap the tree's strings, keys or node storage;
 * do not mutate a tree concurrently with serialisation or other operations.
 * Nesting is limited to 256 containers. Embedded NUL strings/keys are unsupported
 * by this char-pointer representation and are rejected rather than truncated.
 * Floating conversion requires a dot-decimal LC_NUMERIC locale; no global locale
 * changes are made. Non-finite values and numeric range errors are rejected.
 */
# endif

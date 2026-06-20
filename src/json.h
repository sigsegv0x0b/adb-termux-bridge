#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_STRING,
    JSON_NUMBER,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_BOOL
} json_type_t;

typedef struct json_value {
    json_type_t type;
    char *string_value;
    double number_value;
    int bool_value;
    struct json_value *parent;
    struct json_value *next;
    struct json_value *child;
    char *key;
} json_value_t;

json_value_t *json_parse(const char *input);
const char *json_get_string(const json_value_t *obj, const char *key);
double json_get_number(const json_value_t *obj, const char *key);
int json_get_bool(const json_value_t *obj, const char *key);
void json_free(json_value_t *val);

json_value_t *json_new_object(void);
json_value_t *json_new_string(const char *s);
json_value_t *json_new_number(double n);
json_value_t *json_new_bool(int b);
void json_add(json_value_t *obj, const char *key, json_value_t *val);
void json_add_string(json_value_t *obj, const char *key, const char *val);
void json_add_number(json_value_t *obj, const char *key, double val);
void json_add_bool(json_value_t *obj, const char *key, int val);
char *json_serialize(const json_value_t *val);

#endif

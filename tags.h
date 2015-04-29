/* tags.h */
#ifndef TAGS_H_INCLUDED
#define TAGS_H_INCLUDED

#include <stdint.h>
#include "pbf.h"

typedef struct {
    char *key;
    char *val;
} KeyVal;

int8_t encode_tag (ProtobufCBinaryData key, ProtobufCBinaryData val);
size_t decode_tag (char *buf, KeyVal *kv);

uint8_t encode_role (ProtobufCBinaryData role);
char *decode_role (uint8_t code);

#endif /* TAGS_H_INCLUDED */

#pragma once

#include "fs_port.h"
#include "error.h"

typedef struct
{
    bool_t live;
    bool_t nocloud;
    char *source;
    size_t skip_seconds;
    bool_t cache;

    bool_t _stream;
    char *_streamFile;
    uint32_t _version;
    bool_t _updated;

} contentJson_t;

#define CONTENT_JSON_VERSION 2

error_t load_content_json(const char *content_path, contentJson_t *content_json);
error_t save_content_json(const char *content_path, contentJson_t *content_json);
void free_content_json(contentJson_t *content_json);
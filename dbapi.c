#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <iconv.h>
#include <oauth.h>
#include <pthread.h>

#include "dbapi.h"
#include "dbfat.h"
#include "cJSON.h"

char *CONSUMER_KEY_APP_FOLDER    = "ow8tibho1dgcmcl";
char *CONSUMER_SECRET_APP_FOLDER = "5l5dr2ptiwsjn7t";
char *CONSUMER_KEY    = NULL;
char *CONSUMER_SECRET = NULL;

char *TOKEN_KEY    = "2jfilheijeqt4p6";
char *TOKEN_SECRET = "yjn1qn3y5kktpej";

long int REQUEST_TIMEOUT = 60; // in seconds
char *URL_DELTA = "https://api.dropbox.com/1/delta";
char *URL_FILES = "https://api-content.dropbox.com/1/files";

char *DROPBOX_ROOT = "/sandbox";

// HTTP Status Codes
const long int HTTP_OK              = 200;
const long int HTTP_PARTIAL_CONTENT = 206;

CURL *dbapi_curl   = NULL;
char *dbapi_cursor = NULL;

char hex_char(int hex_digit) {
    assert(hex_digit >= 0);
    assert(hex_digit < 16);
    if (hex_digit < 10) {
        return '0' + hex_digit;
    } else {
        return 'a' + (hex_digit - 10);
    }
}

char *db_url_escape(char *url) {
    size_t url_size = strlen(url);
    char *buf = (char *)malloc(url_size * 3 + 1);
    memset(buf, 0, url_size * 3 + 1);

    size_t buf_offset = 0;
    for (int i = 0; i < url_size; i++) {
        // unescaped chars [0-9a-zA-Z], '-', '.', '_' and
        // '/' to be similar to urllib.quote function in python
        if ((48 <= url[i] && url[i] <= 57) ||
            (65 <= url[i] && url[i] <= 90) ||
            (97 <= url[i] && url[i] <= 122) ||
            (url[i] == '-' || url[i] == '.' || url[i] == '_' || url[i] =='/')) {
            buf[buf_offset] = url[i];
            buf_offset += 1;
        } else {
            buf[buf_offset] = '%';
            buf[buf_offset + 1] = hex_char(url[i] / 16);
            buf[buf_offset + 2] = hex_char(url[i] % 16);
            buf_offset += 3;
        }
    }
    buf[buf_offset] = '\x00';
    return buf;
}

void curl_base_setup(CURL *curl) {
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0); //TODO(zm): fix this
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2);

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
}

CURLcode dbapi_request(
        CURL *curl, char* url, const char* method, char *range, char *request_args,
        long int *http_status, char **response_buffer, size_t *response_size
        ) {
    const char *locale = "&locale=en";
    char *posturl = (char *)malloc(strlen(url) + strlen(request_args) + strlen(locale) + 2);
    sprintf(posturl, "%s?%s%s", url, request_args, locale);

    char *signed_url = NULL;
    char *signed_postargs = NULL;
    if (strcmp(method, "GET") == 0) {
        signed_url = oauth_sign_url2(posturl, NULL, OA_HMAC, method, CONSUMER_KEY, CONSUMER_SECRET, TOKEN_KEY, TOKEN_SECRET);
    } else if (strcmp(method, "POST") == 0) {
        signed_url = oauth_sign_url2(posturl, &signed_postargs, OA_HMAC, method, CONSUMER_KEY, CONSUMER_SECRET, TOKEN_KEY, TOKEN_SECRET);
        assert(signed_postargs);
    } else {
        // invalid curl method
        assert(0);
    }
    assert(signed_url);

    curl_base_setup(curl);
    curl_easy_setopt(curl, CURLOPT_URL, signed_url);
    if (strcmp(method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, signed_postargs);
    }

    if (range != NULL) {
        curl_easy_setopt(curl, CURLOPT_RANGE, range);
    }

    printf("[DEBUG] DBApi %s request, signed_url: %s, signed_args: %s\n", method, signed_url, (signed_postargs ? signed_postargs : "NULL"));

    *http_status = 0;
    *response_size  = 0;
    *response_buffer = NULL;
    FILE *response_file = open_memstream(response_buffer, response_size);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_file);
    CURLcode ret = curl_easy_perform(curl);
    fclose(response_file);

    if (ret == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
        printf("[DEBUG] DBApi %s request finished, http status code: %ld, response size: %zu bytes\n", method, *http_status, *response_size);
    } else {
        printf("[ERROR] DBApi %s request failed, error code: %u, error msg: %s\n", method, ret, curl_easy_strerror(ret));
    }

    free(posturl);
    if (signed_postargs) free(signed_postargs);
    if (signed_url) free(signed_url);
    return ret;
}

CURLcode dbapi_json_request(CURL *curl, char* url, const char* method, char *request_args, long int *http_status, cJSON **result) {
    char *response_buffer;
    size_t response_size;
    CURLcode ret = dbapi_request(curl, url, method, NULL, request_args, http_status, &response_buffer, &response_size);
    if (ret == CURLE_OK) {
        *result = cJSON_Parse(response_buffer);
    } else {
        *result = NULL;
    }
    if (response_buffer) free(response_buffer);
    return ret;
}

CURLcode dbapi_range_request(CURL *curl, char* url, const char* method,
        char *request_args, long int range_start, long int range_end,
        long int *http_status, char **response_buffer, size_t *response_size, cJSON **error) {
    char range[20];
    sprintf(range, "%ld-%ld", range_start, range_end - 1);
    CURLcode ret = dbapi_request(curl, url, method, range, request_args, http_status, response_buffer, response_size);
    if (ret == CURLE_OK) {
        if ((*http_status == HTTP_OK) || (*http_status == HTTP_PARTIAL_CONTENT)) {
            *error = NULL;
        } else {
            *error = cJSON_Parse(*response_buffer);
            free(*response_buffer);
            *response_buffer = NULL;
        }
    } else {
        *error = NULL;
        if (*response_buffer) {
            free(*response_buffer);
            *response_buffer = NULL;
        }
    }
    return ret;
}

int dbapi_delta(CURL *curl, char *cursor, long int *http_status, cJSON **result) {
    const char *cursor_prefix = "cursor=";
    char *request_args = (char *)malloc(strlen(cursor_prefix) + strlen(cursor) + 1);
    sprintf(request_args, "%s%s", cursor_prefix, cursor);
    CURLcode ret = dbapi_json_request(curl, URL_DELTA, "POST", request_args, http_status, result);
    free(request_args);
    return ((ret == CURLE_OK) && (*http_status == HTTP_OK)) ? 0 : -1;
}

int dbapi_get_file(CURL *curl, char *path, char *rev, long int range_start, long int range_end,
        char **response_buffer, size_t *response_size) {
    const char *rev_prefix = "rev=";
    char *request_args = (char *)malloc(strlen(rev_prefix) + strlen(rev) + 1);
    sprintf(request_args, "%s%s", rev_prefix, rev);

    char *escaped_path = db_url_escape(path);
    char *url = (char *)malloc(strlen(URL_FILES) + strlen(DROPBOX_ROOT) + strlen(escaped_path) + 1);
    sprintf(url, "%s%s%s", URL_FILES, DROPBOX_ROOT, escaped_path);

    long int http_status;
    cJSON *error = NULL;

    CURLcode ret = dbapi_range_request(
            curl, url, "GET", request_args, range_start, range_end,
            &http_status, response_buffer, response_size, &error
            );

    free(escaped_path);
    free(url);
    free(request_args);
    // TODO(ZM): might want to propagate or print this error
    if (error) cJSON_Delete(error);
    return ((ret == CURLE_OK) && ((http_status == HTTP_OK) || (http_status == HTTP_PARTIAL_CONTENT))) ? 0 : -1;
}

void update_cursor(char *new_cursor) {
    size_t cursor_length = strlen(new_cursor) + 1;
    dbapi_cursor = realloc(dbapi_cursor, cursor_length + 1);
    memcpy(dbapi_cursor, new_cursor, cursor_length);
}

int dbapi_update() {
    long int http_status;
    cJSON *result;
    int ret = dbapi_delta(dbapi_curl, dbapi_cursor, &http_status, &result);

    int update_more = 0;
    if (ret == 0) {
        int reset        = cJSON_GetObjectItem(result, "reset")->valueint;
        int has_more     = cJSON_GetObjectItem(result, "has_more")->valueint;
        char *new_cursor = cJSON_GetObjectItem(result, "cursor")->valuestring;

        if (reset) {
            // remove all file entries from local state
            remove_all_file_entries();
        }

        if (has_more) {
            update_more = 1;
        }

        cJSON *entries = cJSON_GetObjectItem(result, "entries");
        int nentries = cJSON_GetArraySize(entries);
        printf("[DEBUG] DBApi DELTA request, reset: %d, has_more: %d, entries: %d, cursor:\n%s\n",
                reset, has_more, nentries, new_cursor);
        for (int i = 0; i < nentries; i++) {
            cJSON *entry = cJSON_GetArrayItem(entries, i);

            char *path = cJSON_GetArrayItem(entry, 0)->valuestring;
            cJSON *metadata = cJSON_GetArrayItem(entry, 1);

            utf16_t *utf16path;
            size_t utf16path_chars;
            utf8_to_utf16(strlen(path), path, &utf16path_chars, &utf16path);

            if (metadata->type == cJSON_Object) {
                struct DBMetaData dbmetadata;
                dbmetadata.is_dir = (uint8_t)cJSON_GetObjectItem(metadata, "is_dir")->valueint;
                dbmetadata.mtime = (uint32_t)cJSON_GetObjectItem(metadata, "modified")->valuedouble;
                strcpy((char *)dbmetadata.rev, cJSON_GetObjectItem(metadata, "rev")->valuestring);

                double size_double = cJSON_GetObjectItem(metadata, "bytes")->valuedouble;
                if (size_double > FAT_MAX_FILE_SIZE) {
                    // truncate files that are larger than what is supported by FAT
                    dbmetadata.size = FAT_MAX_FILE_SIZE;
                } else {
                    dbmetadata.size = (uint32_t)size_double;
                }

                printf("ENTRY: %s\tis_dir: %u\tmtime: %u\tsize: %u, rev: %s\n",
                        path, dbmetadata.is_dir, dbmetadata.mtime, dbmetadata.size, dbmetadata.rev);
                add_file_entry(utf16path_chars, utf16path, &dbmetadata);
            } else {
                printf("ENTRY: %s\tremoved\n", path);
                remove_file_entry(utf16path_chars, utf16path);
            }

            free(utf16path);
        }

        // update the dbapi_cursor
        update_cursor(new_cursor);

        cJSON_Delete(result);
    } else {
        update_more = 2;
    }

    return update_more;
}

void *dbapi_thread(void *args) {
    curl_global_init(CURL_GLOBAL_ALL);
    dbapi_curl = curl_easy_init();
    dbapi_cursor = calloc(1, sizeof(char));

    CONSUMER_KEY    = CONSUMER_KEY_APP_FOLDER;
    CONSUMER_SECRET = CONSUMER_SECRET_APP_FOLDER;

    while (1) {
        int update_more = dbapi_update();
        switch (update_more) {
            case 0:
            case 2:
                sleep(600);
                break;
        }
    }
}

void start_dbapi_thread() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 128 * 1024);

    pthread_t thread;
    pthread_create(&thread, &attr, dbapi_thread, NULL);
}

void dbapi_test() {
    curl_global_init(CURL_GLOBAL_ALL);
    dbapi_curl = curl_easy_init();
    dbapi_cursor = calloc(1, sizeof(char));

    CONSUMER_KEY    = CONSUMER_KEY_APP_FOLDER;
    CONSUMER_SECRET = CONSUMER_SECRET_APP_FOLDER;
    dbapi_update();

    curl_easy_cleanup(dbapi_curl);
}

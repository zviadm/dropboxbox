#include <assert.h>
#include <curl/curl.h>
#include <iconv.h>
#include <oauth.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// HTTP Status Codes
const long int HTTP_OK = 200;

CURL *dbapi_curl   = NULL;
char *dbapi_cursor = "";

void curl_base_setup(CURL *curl) {
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0); //TODO(zm): fix this
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2);

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
}
//
//void get_oauth_header(char *url, char *method, char *postparams, ) {
//    const char *consumer_key_prefix = "&oauth_consumer_key=";
//    const char *token_prefix        = "&oauth_token=";
//    const char *timestamp_prefix    = "&oauth_timestamp=";
//    const char *nonce_prefix        = "&oauth_nonce=";
//    const char *version             = "&oauth_version=1.0";
//   
//    char *timestamp = (char *)malloc(16);
//    sprintf(timestamp, "%ld", time(NULL));
//    char *nonce = oauth_gen_nonce();
//    char *oauth_params = (char *)malloc(
//            strlen(postdata)            +
//            strlen(consumer_key_prefix) + strlen(CONSUMER_KEY) +
//            strlen(token_prefix)        + strlen(TOKEN_KEY)    +
//            strlen(timestamp_prefix)    + strlen(timestamp)    +
//            strlen(nonce_prefix)        + strlen(nonce)        +
//            strlen(version)             +
//            1);
//    sprintf(oauth_params, "%s%s%s%s%s%s%s%s%s%s", 
//            postdata,
//            consumer_key_prefix, CONSUMER_KEY,
//            token_prefix       , TOKEN_KEY,
//            timestamp_prefix   , timestamp,
//            nonce_prefix       , nonce,
//            version);
//
//    char *signed_params = oauth_params;
//    oauth_sign_url2(url, &signed_params, OA_HMAC, method, CONSUMER_KEY, CONSUMER_SECRET, TOKEN_KEY, TOKEN_SECRET);
//
//    free(timestamp);
//    free(nonce);
//    free(oauth_params);
//}
//
CURLcode dbapi_post_request(CURL *curl, char* url, char *postargs, long int *http_status, cJSON **result) {
    const char *locale = "&locale=en";
    char *postargs2 = (char *)malloc(strlen(postargs) + strlen(locale) + 1);
    sprintf(postargs2, "%s%s", postargs, locale);

    char *signed_postargs = postargs2;
    char *signed_url = oauth_sign_url2(url, &signed_postargs, OA_HMAC, "POST", CONSUMER_KEY, CONSUMER_SECRET, TOKEN_KEY, TOKEN_SECRET);

    curl_base_setup(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, signed_postargs);

    *http_status = 0;
    *result = NULL;

    char *response_buffer = NULL;
    size_t response_size  = 0;
    FILE *response_file = open_memstream(&response_buffer, &response_size);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_file);
    CURLcode ret = curl_easy_perform(curl);
    if (ret == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
        if (*http_status == HTTP_OK) {
            fflush(response_file);
            printf("[DEBUG] DBApi POST request succeeded, response size: %zu bytes, response:\n%s\n", response_size, response_buffer);
	        *result = cJSON_Parse(response_buffer);
        } else {
            printf("[ERROR] DBApi POST request failed, http status code: %ld\n", *http_status);
        }
    } else {
        printf("[ERROR] DBApi POST request failed, error code: %u, error msg: %s\n", 
                ret, curl_easy_strerror(ret));
    }

    fclose(response_file);
    free(response_buffer);
    free(postargs2);
    if (signed_postargs) free(signed_postargs);
    if (signed_url) free(signed_url);
    return ret;
}

CURLcode dbapi_delta(CURL *curl, char *cursor, long int *http_status, cJSON **result) {
    const char *cursor_prefix = "cursor=";
    char *postargs = (char *)malloc(strlen(cursor_prefix) + strlen(cursor) + 1);
    sprintf(postargs, "%s%s", cursor_prefix, cursor);
    CURLcode ret = dbapi_post_request(curl, URL_DELTA, postargs, http_status, result);
    free(postargs);
    return ret;
}

void dbapi_update() {
    long int http_status;
    cJSON *result;
    CURLcode ret = dbapi_delta(dbapi_curl, dbapi_cursor, &http_status, &result);

    if (result) {
        assert(ret == CURLE_OK);
        assert(http_status == HTTP_OK);

        int reset        = cJSON_GetObjectItem(result, "reset")->valueint;
        int has_more     = cJSON_GetObjectItem(result, "has_more")->valueint;
        char *new_cursor = cJSON_GetObjectItem(result, "cursor")->valuestring;

        if (reset) {
            // TODO(zm): remove all files and reset state
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

            if (metadata) {
                struct DBMetaData dbmetadata;
                dbmetadata.is_dir = (uint8_t)cJSON_GetObjectItem(metadata, "is_dir")->valueint;
                dbmetadata.mtime = (uint32_t)cJSON_GetObjectItem(metadata, "modified")->valuedouble;

                double size_double = cJSON_GetObjectItem(metadata, "bytes")->valuedouble;
                if (size_double > FAT_MAX_FILE_SIZE) {
                    // truncate files that are larger than what is supported by FAT
                    dbmetadata.size = FAT_MAX_FILE_SIZE; 
                } else {
                    dbmetadata.size = (uint32_t)size_double;
                }

                printf("ENTRY: %s\tis_dir: %u\tmtime: %u\tsize: %u\n", 
                        path, dbmetadata.is_dir, dbmetadata.mtime, dbmetadata.size);
                add_file_entry(utf16path_chars, utf16path, &dbmetadata);
            } else {
                printf("ENTRY: %s\tremoved\n", path);
                remove_file_entry(utf16path_chars, utf16path);
            }

            free(utf16path);
        }
    }

    cJSON_Delete(result);
}

void dbapi_test() {
    curl_global_init(CURL_GLOBAL_ALL);
    dbapi_curl = curl_easy_init();

    CONSUMER_KEY    = CONSUMER_KEY_APP_FOLDER;
    CONSUMER_SECRET = CONSUMER_SECRET_APP_FOLDER;
    dbapi_update();

    curl_easy_cleanup(dbapi_curl);
}

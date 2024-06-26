/** **************************************************************************
 * request.c
 * 
 * Copyright 2008 Bryan Ischo <bryan@ischo.com>
 * 
 * This file is part of libs3.
 * 
 * libs3 is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, version 3 of the License.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of this library and its programs with the
 * OpenSSL library, and distribute linked combinations including the two.
 *
 * libs3 is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * version 3 along with libs3, in a file named COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 ************************************************************************** **/

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>
#include "request.h"
#include "request_context.h"
#include "response_headers_handler.h"
#include "util.h"
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/x509v3.h>
#include <openssl/ssl.h>
#include <unistd.h>

#define USER_AGENT_SIZE 256
#define REQUEST_STACK_SIZE 32
static int verifyPeer;

static S3SignatureVersion signatureVersionG;

static char userAgentG[USER_AGENT_SIZE];

static pthread_mutex_t requestStackMutexG;

static Request *requestStackG[REQUEST_STACK_SIZE];

static int requestStackCountG;

char defaultHostNameG[S3_MAX_HOSTNAME_SIZE];
char whichRegionIsHereG[S3_MAX_HOSTNAME_SIZE] = "us-east-1";
char caInfoG[S3_MAX_HOSTNAME_SIZE] = {0};

typedef struct RequestComputedValues
{
    // All x-amz- headers, in normalized form (i.e. NAME: VALUE, no other ws)
    char *amzHeaders[S3_MAX_METADATA_COUNT + 2]; // + 2 for acl and date

    // The number of x-amz- headers
    int amzHeadersCount;

    // Storage for amzHeaders (the +256 is for x-amz-acl and x-amz-date)
    // +4096 is for the possibility of x-amz-security-token
    char amzHeadersRaw[COMPACTED_METADATA_BUFFER_SIZE + 256 + 4096 + 1];

    // Canonicalized x-amz- headers
    string_multibuffer(canonicalizedAmzHeaders,
                       COMPACTED_METADATA_BUFFER_SIZE + 256 + 4096 + 1);

    // URL-Encoded key
    char urlEncodedKey[MAX_URLENCODED_KEY_SIZE + 1];

    // Canonicalized resource
    char canonicalizedResource[MAX_CANONICALIZED_RESOURCE_SIZE + 1];

    // Cache-Control header (or empty)
    char cacheControlHeader[128];

    // Content-Type header (or empty)
    char contentTypeHeader[128];

    // Content-MD5 header (or empty)
    char md5Header[128];

    // Content-Disposition header (or empty)
    char contentDispositionHeader[128];

    // Content-Encoding header (or empty)
    char contentEncodingHeader[128];

    // Expires header (or empty)
    char expiresHeader[128];

    // If-Modified-Since header
    char ifModifiedSinceHeader[128];

    // If-Unmodified-Since header
    char ifUnmodifiedSinceHeader[128];

    // If-Match header
    char ifMatchHeader[128];

    // If-None-Match header
    char ifNoneMatchHeader[128];

    // Range header
    char rangeHeader[128];

    // Authorization header
    char authorizationHeader[128];

    // Host header
    char hostHeader[128];

    // Time stamp is ISO 8601 format: 'yyyymmddThhmmssZ'
    char timestamp[32];

    // The signed headers
    char signedHeaders[COMPACTED_METADATA_BUFFER_SIZE];
} RequestComputedValues;


// Called whenever we detect that the request headers have been completely
// processed; which happens either when we get our first read/write callback,
// or the request is finished being procesed.  Returns nonzero on success,
// zero on failure.
static void request_headers_done(Request *request)
{
    if (request->propertiesCallbackMade) {
        return;
    }

    request->propertiesCallbackMade = 1;

    // Get the http response code
    long httpResponseCode;
    request->httpResponseCode = 0;
    CURLcode status;
    if ((status = curl_easy_getinfo(request->curl, CURLINFO_RESPONSE_CODE, 
                                    &httpResponseCode)) != CURLE_OK) {
        fprintf(stderr, "request_headers_done.curl_easy_getinfo: %s\n", curl_easy_strerror(status));
        // Not able to get the HTTP response code - error
        request->status = S3StatusInternalError;
        return;
    }
    else {
        request->httpResponseCode = httpResponseCode;
    }

    response_headers_handler_done(&(request->responseHeadersHandler), 
                                  request->curl);

    // Only make the callback if it was a successful request; otherwise we're
    // returning information about the error response itself
    if (request->propertiesCallback &&
        (request->httpResponseCode >= 200) &&
        (request->httpResponseCode <= 299)) {
        request->status = (*(request->propertiesCallback))
            (&(request->responseHeadersHandler.responseProperties), 
             request->callbackData);
    }
}


static size_t curl_header_func(void *ptr, size_t size, size_t nmemb,
                               void *data)
{
    Request *request = (Request *) data;

    int len = size * nmemb;

    response_headers_handler_add
        (&(request->responseHeadersHandler), (char *) ptr, len);

    return len;
}


static size_t curl_read_func(void *ptr, size_t size, size_t nmemb, void *data)
{
    Request *request = (Request *) data;

    int len = size * nmemb;

    // CURL may call this function before response headers are available,
    // so don't assume response headers are available and attempt to parse
    // them.  Leave that to curl_write_func, which is guaranteed to be called
    // only after headers are available.

    if (request->status != S3StatusOK) {
        return CURL_READFUNC_ABORT;
    }

    // If there is no data callback, or the data callback has already returned
    // contentLength bytes, return 0;
    if (!request->toS3Callback || !request->toS3CallbackBytesRemaining) {
        return 0;
    }
    
    // Don't tell the callback that we are willing to accept more data than we
    // really are
    if (len > request->toS3CallbackBytesRemaining) {
        len = request->toS3CallbackBytesRemaining;
    }

    // Otherwise, make the data callback
    int ret = (*(request->toS3Callback))
        (len, (char *) ptr, request->callbackData);
    if (ret < 0) {
        request->status = S3StatusAbortedByCallback;
        return CURL_READFUNC_ABORT;
    }
    else {
        if (ret > request->toS3CallbackBytesRemaining) {
            ret = request->toS3CallbackBytesRemaining;
        }
        request->toS3CallbackBytesRemaining -= ret;
        return ret;
    }
}


static size_t curl_write_func(void *ptr, size_t size, size_t nmemb,
                              void *data)
{
    Request *request = (Request *) data;

    int len = size * nmemb;

    request_headers_done(request);

    if (request->status != S3StatusOK) {
        return 0;
    }

    // On HTTP error, we expect to parse an HTTP error response
    if ((request->httpResponseCode < 200) || 
        (request->httpResponseCode > 299)) {
        request->status = error_parser_add
            (&(request->errorParser), (char *) ptr, len);
    }
    // If there was a callback registered, make it
    else if (request->fromS3Callback) {
        request->status = (*(request->fromS3Callback))
            (len, (char *) ptr, request->callbackData);
    }
    // Else, consider this an error - S3 has sent back data when it was not
    // expected
    else {
        request->status = S3StatusInternalError;
    }

    return ((request->status == S3StatusOK) ? len : 0);
}


// This function 'normalizes' all x-amz-meta headers provided in
// params->requestHeaders, which means it removes all whitespace from
// them such that they all look exactly like this:
// x-amz-meta-${NAME}: ${VALUE}
// It also adds the x-amz-acl, x-amz-copy-source, x-amz-metadata-directive,
// and x-amz-server-side-encryption headers if necessary, and always adds the
// x-amz-date header.  It copies the raw string values into
// params->amzHeadersRaw, and creates an array of string pointers representing
// these headers in params->amzHeaders (and also sets params->amzHeadersCount
// to be the count of the total number of x-amz- headers thus created).
static S3Status compose_amz_headers(const RequestParams *params,
                                    RequestComputedValues *values)
{
    const S3PutProperties *properties = params->putProperties;

    values->amzHeadersCount = 0;
    values->amzHeadersRaw[0] = 0;
    int len = 0;

    // Append a header to amzHeaders, trimming whitespace from the end.
    // Does NOT trim whitespace from the beginning.
#define headers_append(isNewHeader, format, ...)                        \
    do {                                                                \
        if (isNewHeader) {                                              \
            values->amzHeaders[values->amzHeadersCount++] =             \
                &(values->amzHeadersRaw[len]);                          \
        }                                                               \
        len += snprintf(&(values->amzHeadersRaw[len]),                  \
                        sizeof(values->amzHeadersRaw) - len,            \
                        format, __VA_ARGS__);                           \
        if (len >= (int) sizeof(values->amzHeadersRaw)) {               \
            return S3StatusMetaDataHeadersTooLong;                      \
        }                                                               \
        while ((len > 0) && (values->amzHeadersRaw[len - 1] == ' ')) {  \
            len--;                                                      \
        }                                                               \
        values->amzHeadersRaw[len++] = 0;                               \
    } while (0)

#define header_name_tolower_copy(str, l)                                \
    do {                                                                \
        values->amzHeaders[values->amzHeadersCount++] =                 \
            &(values->amzHeadersRaw[len]);                              \
        if ((len + l) >= (int) sizeof(values->amzHeadersRaw)) {         \
            return S3StatusMetaDataHeadersTooLong;                      \
        }                                                               \
        int todo = l;                                                   \
        while (todo--) {                                                \
            if ((*(str) >= 'A') && (*(str) <= 'Z')) {                   \
                values->amzHeadersRaw[len++] = 'a' + (*(str) - 'A');    \
            }                                                           \
            else {                                                      \
                values->amzHeadersRaw[len++] = *(str);                  \
            }                                                           \
            (str)++;                                                    \
        }                                                               \
    } while (0)

    // Check and copy in the x-amz-meta headers
    if (properties) {
        int i;
        for (i = 0; i < properties->metaDataCount; i++) {
            const S3NameValue *property = &(properties->metaData[i]);
            char headerName[S3_MAX_METADATA_SIZE - sizeof(": v")];
            int l = 0;
            if (strcmp(property->name, S3_TAGGING_DIRECTIVE) == 0) {
                l = snprintf(headerName, sizeof(headerName), "%s", S3_TAGGING_HEADER_NAME);
            } else {
                l = snprintf(headerName, sizeof(headerName),
                             S3_METADATA_HEADER_NAME_PREFIX "%s",
                             property->name);
            }
            char *hn = headerName;
            header_name_tolower_copy(hn, l);
            // Copy in the value
            headers_append(0, ": %s", property->value);
        }

        // Add the x-amz-acl header, if necessary
        const char *cannedAclString;
        switch (properties->cannedAcl) {
        case S3CannedAclPrivate:
            cannedAclString = 0;
            break;
        case S3CannedAclPublicRead:
            cannedAclString = "public-read";
            break;
        case S3CannedAclPublicReadWrite:
            cannedAclString = "public-read-write";
            break;
        default: // S3CannedAclAuthenticatedRead
            cannedAclString = "authenticated-read";
            break;
        }
        if (cannedAclString) {
            headers_append(1, "x-amz-acl: %s", cannedAclString);
        }

        // Add the x-amz-server-side-encryption header, if necessary
        if (properties->useServerSideEncryption) {
            headers_append(1, "x-amz-server-side-encryption: %s", "AES256");
        }
    }

    // Add the x-amz-date header
    time_t now = time(NULL);
    char date[64];
    struct tm gmt;
    if (signatureVersionG == S3SignatureV2) {
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT",
                 gmtime_r(&now, &gmt));
    } else {
        // Auth version 4 requires ISO 8601 date time format 
        strftime(date, sizeof(date), "%Y%m%dT%H%M%SZ", gmtime_r(&now, &gmt));
        memcpy(values->timestamp, date, sizeof(values->timestamp));
    }
    headers_append(1, "x-amz-date: %s", date);

    if (signatureVersionG == S3SignatureV4) {
        // Add the x-amz-content-sha256 header
        if (properties && properties->md5 && properties->md5[0]) {
            headers_append(
                1, "x-amz-content-sha256: %s",
                properties->md5 + 1 + strlen(properties->md5)
            );
        } else {
            headers_append(1, "%s", "x-amz-content-sha256: UNSIGNED-PAYLOAD");
        }
    }
    if (params->httpRequestType == HttpRequestTypeCOPY) {
        // Add the x-amz-copy-source header
        if (params->copySourceBucketName && params->copySourceBucketName[0] &&
            params->copySourceKey && params->copySourceKey[0]) {
            headers_append(1, "x-amz-copy-source: /%s/%s",
                           params->copySourceBucketName,
                           params->copySourceKey);
        }
        // If byteCount != 0 then we're just copying a range, add header
        if (params->byteCount > 0) {
            headers_append(1, "x-amz-copy-source-range: bytes=%ld-%ld",
                           (long) params->startByte,
                           (long) (params->startByte + params->byteCount));
        }
        // And the x-amz-metadata-directive header
        if (properties) {
            headers_append(1, "%s", "x-amz-metadata-directive: REPLACE");
        }
    }

    // Add the x-amz-security-token header if necessary
    if (params->bucketContext.securityToken) {
        headers_append(1, "x-amz-security-token: %s",
                       params->bucketContext.securityToken);
    }

    return S3StatusOK;
}


// Composes the other headers
static S3Status compose_standard_headers(const RequestParams *params,
                                         RequestComputedValues *values)
{

#define do_put_header(fmt, sourceField, destField, badError, tooLongError)  \
    do {                                                                    \
        if (params->putProperties &&                                        \
            params->putProperties-> sourceField &&                          \
            params->putProperties-> sourceField[0]) {                       \
            /* Skip whitespace at beginning of val */                       \
            const char *val = params->putProperties-> sourceField;          \
            while (*val && is_blank(*val)) {                                \
                val++;                                                      \
            }                                                               \
            if (!*val) {                                                    \
                return badError;                                            \
            }                                                               \
            /* Compose header, make sure it all fit */                      \
            int len = snprintf(values-> destField,                          \
                               sizeof(values-> destField), fmt, val);       \
            if (len >= (int) sizeof(values-> destField)) {                  \
                return tooLongError;                                        \
            }                                                               \
            /* Now remove the whitespace at the end */                      \
            while (is_blank(values-> destField[len])) {                     \
                len--;                                                      \
            }                                                               \
            values-> destField[len] = 0;                                    \
        }                                                                   \
        else {                                                              \
            values-> destField[0] = 0;                                      \
        }                                                                   \
    } while (0)

#define do_get_header(fmt, sourceField, destField, badError, tooLongError)  \
    do {                                                                    \
        if (params->getConditions &&                                        \
            params->getConditions-> sourceField &&                          \
            params->getConditions-> sourceField[0]) {                       \
            /* Skip whitespace at beginning of val */                       \
            const char *val = params->getConditions-> sourceField;          \
            while (*val && is_blank(*val)) {                                \
                val++;                                                      \
            }                                                               \
            if (!*val) {                                                    \
                return badError;                                            \
            }                                                               \
            /* Compose header, make sure it all fit */                      \
            int len = snprintf(values-> destField,                          \
                               sizeof(values-> destField), fmt, val);       \
            if (len >= (int) sizeof(values-> destField)) {                  \
                return tooLongError;                                        \
            }                                                               \
            /* Now remove the whitespace at the end */                      \
            while (is_blank(values-> destField[len])) {                     \
                len--;                                                      \
            }                                                               \
            values-> destField[len] = 0;                                    \
        }                                                                   \
        else {                                                              \
            values-> destField[0] = 0;                                      \
        }                                                                   \
    } while (0)

    // Host
    if (params->bucketContext.uriStyle == S3UriStyleVirtualHost) {
        const char *requestHostName = params->bucketContext.hostName
                ? params->bucketContext.hostName : defaultHostNameG;

        size_t len = snprintf(values->hostHeader, sizeof(values->hostHeader),
                              "Host: %s.%s", params->bucketContext.bucketName,
                              requestHostName);
        if (len >= sizeof(values->hostHeader)) {
            return S3StatusUriTooLong;
        }
        while (is_blank(values->hostHeader[len])) {
            len--;
        }
        values->hostHeader[len] = 0;
    } else if (params->bucketContext.hostHeaderValue && params->bucketContext.hostHeaderValue[0]) {
        // forced value for Host header
        size_t len = snprintf(values->hostHeader, sizeof(values->hostHeader),
                             "Host: %s", params->bucketContext.hostHeaderValue);
        if (len >= sizeof(values->hostHeader)) {
            return S3StatusUriTooLong;
        }
        while (is_blank(values->hostHeader[len])) {
            len--;
        }
        values->hostHeader[len] = 0;
    } else if (signatureVersionG == S3SignatureV4) {
        const char *requestHostName = params->bucketContext.hostName
                ? params->bucketContext.hostName : defaultHostNameG;
        snprintf(values->hostHeader, sizeof(values->hostHeader),
                 "Host: %s", requestHostName);
    } else {
        values->hostHeader[0] = 0;
    }

    // Cache-Control
    do_put_header("Cache-Control: %s", cacheControl, cacheControlHeader,
                  S3StatusBadCacheControl, S3StatusCacheControlTooLong);
    
    // ContentType
    do_put_header("Content-Type: %s", contentType, contentTypeHeader,
                  S3StatusBadContentType, S3StatusContentTypeTooLong);

    // MD5
    do_put_header("Content-MD5: %s", md5, md5Header, S3StatusBadMD5,
                  S3StatusMD5TooLong);

    // Content-Disposition
    do_put_header("Content-Disposition: attachment; filename=\"%s\"",
                  contentDispositionFilename, contentDispositionHeader,
                  S3StatusBadContentDispositionFilename,
                  S3StatusContentDispositionFilenameTooLong);
    
    // ContentEncoding
    do_put_header("Content-Encoding: %s", contentEncoding, 
                  contentEncodingHeader, S3StatusBadContentEncoding,
                  S3StatusContentEncodingTooLong);
    
    // Expires
    if (params->putProperties && (params->putProperties->expires >= 0)) {
        time_t t = (time_t) params->putProperties->expires;
        struct tm gmt;
        strftime(values->expiresHeader, sizeof(values->expiresHeader),
                 "Expires: %a, %d %b %Y %H:%M:%S UTC", gmtime_r(&t, &gmt));
    }
    else {
        values->expiresHeader[0] = 0;
    }

    // If-Modified-Since
    if (params->getConditions &&
        (params->getConditions->ifModifiedSince >= 0)) {
        time_t t = (time_t) params->getConditions->ifModifiedSince;
        struct tm gmt;
        strftime(values->ifModifiedSinceHeader,
                 sizeof(values->ifModifiedSinceHeader),
                 "If-Modified-Since: %a, %d %b %Y %H:%M:%S UTC", gmtime_r(&t, &gmt));
    }
    else {
        values->ifModifiedSinceHeader[0] = 0;
    }

    // If-Unmodified-Since header
    if (params->getConditions &&
        (params->getConditions->ifNotModifiedSince >= 0)) {
        time_t t = (time_t) params->getConditions->ifNotModifiedSince;
        struct tm gmt;
        strftime(values->ifUnmodifiedSinceHeader,
                 sizeof(values->ifUnmodifiedSinceHeader),
                 "If-Unmodified-Since: %a, %d %b %Y %H:%M:%S UTC", gmtime_r(&t, &gmt));
    }
    else {
        values->ifUnmodifiedSinceHeader[0] = 0;
    }
    
    // If-Match header
    do_get_header("If-Match: %s", ifMatchETag, ifMatchHeader,
                  S3StatusBadIfMatchETag, S3StatusIfMatchETagTooLong);
    
    // If-None-Match header
    do_get_header("If-None-Match: %s", ifNotMatchETag, ifNoneMatchHeader,
                  S3StatusBadIfNotMatchETag, 
                  S3StatusIfNotMatchETagTooLong);
    
    // Range header
    if (params->startByte || params->byteCount) {
        if (params->byteCount) {
            snprintf(values->rangeHeader, sizeof(values->rangeHeader),
                     "Range: bytes=%llu-%llu", 
                     (unsigned long long) params->startByte,
                     (unsigned long long) (params->startByte + 
                                           params->byteCount - 1));
        }
        else {
            snprintf(values->rangeHeader, sizeof(values->rangeHeader),
                     "Range: bytes=%llu-", 
                     (unsigned long long) params->startByte);
        }
    }
    else {
        values->rangeHeader[0] = 0;
    }

    return S3StatusOK;
}


// URL encodes the params->key value into params->urlEncodedKey
static S3Status encode_key(const RequestParams *params,
                           RequestComputedValues *values)
{
    return (urlEncode(values->urlEncodedKey, params->key, S3_MAX_KEY_SIZE) ?
            S3StatusOK : S3StatusUriTooLong);
}


// Simple comparison function for comparing two HTTP header names that are
// embedded within an HTTP header line, returning true if header1 comes
// before header2 alphabetically, false if not
static int headerle(const char *header1, const char *header2)
{
    while (1) {
        if (*header1 == ':') {
            return (*header2 != ':');
        }
        else if (*header2 == ':') {
            return 0;
        }
        else if (*header2 < *header1) {
            return 0;
        }
        else if (*header2 > *header1) {
            return 1;
        }
        header1++, header2++;
    }
}


// Replace this with merge sort eventually, it's the best stable sort.  But
// since typically the number of elements being sorted is small, it doesn't
// matter that much which sort is used, and gnome sort is the world's simplest
// stable sort.  Added a slight twist to the standard gnome_sort - don't go
// forward +1, go forward to the last highest index considered.  This saves
// all the string comparisons that would be done "going forward", and thus
// only does the necessary string comparisons to move values back into their
// sorted position.
static void header_gnome_sort(const char **headers, int size)
{
    int i = 0, last_highest = 0;

    while (i < size) {
        if ((i == 0) || headerle(headers[i - 1], headers[i])) {
            i = ++last_highest;
        }
        else {
            const char *tmp = headers[i];
            headers[i] = headers[i - 1];
            headers[--i] = tmp;
        }
    }
}


// Canonicalizes the x-amz- headers into the canonicalizedAmzHeaders buffer
static void canonicalize_amz_headers(RequestComputedValues *values)
{
    // Make a copy of the headers that will be sorted
    const char *sortedHeaders[S3_MAX_METADATA_COUNT];

    memcpy(sortedHeaders, values->amzHeaders,
           (values->amzHeadersCount * sizeof(sortedHeaders[0])));

    // Now sort these
    header_gnome_sort(sortedHeaders, values->amzHeadersCount);

    // Now copy this sorted list into the buffer, all the while:
    // - folding repeated headers into single lines, and
    // - folding multiple lines
    // - removing the space after the colon
    int lastHeaderLen = 0, i;
    char *buffer = values->canonicalizedAmzHeaders;
    for (i = 0; i < values->amzHeadersCount; i++) {
        const char *header = sortedHeaders[i];
        const char *c = header;
        // If the header names are the same, append the next value
        if ((i > 0) && 
            !strncmp(header, sortedHeaders[i - 1], lastHeaderLen)) {
            // Replacing the previous newline with a comma
            *(buffer - 1) = ',';
            // Skip the header name and space
            c += (lastHeaderLen + 1);
        }
        // Else this is a new header
        else {
            // Copy in everything up to the space in the ": "
            while (*c != ' ') {
                *buffer++ = *c++;
            }
            // Save the header len since it's a new header
            lastHeaderLen = c - header;
            // Skip the space
            c++;
        }
        // Now copy in the value, folding the lines
        while (*c) {
            // If c points to a \r\n[whitespace] sequence, then fold
            // this newline out
            if ((*c == '\r') && (*(c + 1) == '\n') && is_blank(*(c + 2))) {
                c += 3;
                while (is_blank(*c)) {
                    c++;
                }
                // Also, what has most recently been copied into buffer amy
                // have been whitespace, and since we're folding whitespace
                // out around this newline sequence, back buffer up over
                // any whitespace it contains
                while (is_blank(*(buffer - 1))) {
                    buffer--;
                }
                continue;
            }
            *buffer++ = *c++;
        }
        // Finally, add the newline
        *buffer++ = '\n';
    }

    // Terminate the buffer
    *buffer = 0;
}


// Canonicalizes the resource into params->canonicalizedResource
static void canonicalize_resource(const char *bucketName,
                                  const char *subResource,
                                  const char *urlEncodedKey,
                                  char *buffer)
{
    int len = 0;

    *buffer = 0;

#define append(str) len += sprintf(&(buffer[len]), "%s", str)

    if (bucketName && bucketName[0]) {
        buffer[len++] = '/';
        append(bucketName);
    }

    append("/");

    if (urlEncodedKey && urlEncodedKey[0]) {
        append(urlEncodedKey);
    }

    if (subResource && subResource[0]) {
        append("?");
        append(subResource);
    }
}


// Convert an HttpRequestType to an HTTP Verb string
static const char *http_request_type_to_verb(HttpRequestType requestType)
{
    switch (requestType) {
    case HttpRequestTypePOST:
        return "POST";
    case HttpRequestTypeGET:
        return "GET";
    case HttpRequestTypeHEAD:
        return "HEAD";
    case HttpRequestTypePUT:
    case HttpRequestTypeCOPY:
        return "PUT";
    default: // HttpRequestTypeDELETE
        return "DELETE";
    }
}


// Composes the Authorization header for the request
static S3Status compose_auth_header(const RequestParams *params,
                                    RequestComputedValues *values)
{
    // We allow for:
    // 17 bytes for HTTP-Verb + \n
    // 129 bytes for Content-MD5 + \n
    // 129 bytes for Content-Type + \n
    // 1 byte for empty Date + \n
    // CanonicalizedAmzHeaders & CanonicalizedResource
    char signbuf[17 + 129 + 129 + 1 + 
                 (sizeof(values->canonicalizedAmzHeaders) - 1) +
                 (sizeof(values->canonicalizedResource) - 1) + 1];
    int len = 0;

#define signbuf_append(format, ...)                             \
    len += snprintf(&(signbuf[len]), sizeof(signbuf) - len,     \
                    format, __VA_ARGS__)

    signbuf_append
        ("%s\n", http_request_type_to_verb(params->httpRequestType));

    // For MD5 and Content-Type, use the value in the actual header, because
    // it's already been trimmed
    signbuf_append("%s\n", values->md5Header[0] ? 
                   &(values->md5Header[sizeof("Content-MD5: ") - 1]) : "");

    signbuf_append
        ("%s\n", values->contentTypeHeader[0] ? 
         &(values->contentTypeHeader[sizeof("Content-Type: ") - 1]) : "");

    signbuf_append("%s", "\n"); // Date - we always use x-amz-date

    signbuf_append("%s", values->canonicalizedAmzHeaders);

    signbuf_append("%s", values->canonicalizedResource);

    // Generate an HMAC-SHA-1 of the signbuf
    unsigned char hmac[20];

    HMAC_SHA1(hmac, (unsigned char *) params->bucketContext.secretAccessKey,
              strlen(params->bucketContext.secretAccessKey),
              (unsigned char *) signbuf, len);

    // Now base-64 encode the results
    char b64[((20 + 1) * 4) / 3];
    int b64Len = base64Encode(hmac, 20, b64);
    
    snprintf(values->authorizationHeader, sizeof(values->authorizationHeader),
             "Authorization: AWS %s:%.*s", params->bucketContext.accessKeyId,
             b64Len, b64);

    return S3StatusOK;
}


// Compose the URI to use for the request given the request parameters
static S3Status compose_uri(char *buffer, int bufferSize,
                            const S3BucketContext *bucketContext,
                            const char *urlEncodedKey,
                            const char *subResource, const char *queryParams)
{
    int len = 0;
    
#define uri_append(fmt, ...)                                                 \
    do {                                                                     \
        len += snprintf(&(buffer[len]), bufferSize - len, fmt, __VA_ARGS__); \
        if (len >= bufferSize) {                                             \
            return S3StatusUriTooLong;                                       \
        }                                                                    \
    } while (0)

    uri_append("http%s://", 
               (bucketContext->protocol == S3ProtocolHTTP) ? "" : "s");

    const char *hostName = 
        bucketContext->hostName ? bucketContext->hostName : defaultHostNameG;

    if (bucketContext->bucketName && 
        bucketContext->bucketName[0]) {
        if (bucketContext->uriStyle == S3UriStyleVirtualHost) {
            if (strchr(bucketContext->bucketName, '.') == NULL) {
                uri_append("%s.%s", bucketContext->bucketName, hostName);
            }
            else {
                // We'll use the hostName in the URL, and then explicitly set
                // the Host header to match bucket.host so that host validation
                // works.
                uri_append("%s", hostName);
            }
        }
        else {
            uri_append("%s/%s", hostName, bucketContext->bucketName);
        }
    }
    else {
        uri_append("%s", hostName);
    }

    uri_append("%s", "/");

    uri_append("%s", urlEncodedKey);
    
    if (subResource && subResource[0]) {
        uri_append("?%s", subResource);
    }
    
    if (queryParams) {
        uri_append("%s%s", (subResource && subResource[0]) ? "&" : "?",
                   queryParams);
    }
    
    return S3StatusOK;
}

static S3Status string_append_char(char *buf, int maxlen, int *plen, char c)
{
    if (*plen >= maxlen - 1) return S3StatusUriTooLong;
    buf[*plen] = c;
    ++(*plen);
    return S3StatusOK;
}

#define string_appendf(buf, maxlen, plen, format, ...)       do {    \
    *(plen) += snprintf(&((buf)[*(plen)]),                           \
                        (maxlen) - (*(plen)),                        \
                        format, __VA_ARGS__);                        \
    } while (0)

static S3Status string_append_hex(char *buf, int maxlen, int *plen,
                                  const char *data, int datalen)
{
    int i;
    for (i = 0; i < datalen; i++) {
        string_appendf(buf, maxlen, plen, "%02x", (unsigned char)data[i]);
    }
    return S3StatusOK;
}

static S3Status string_append_params(char *buf, int maxlen, int *plen,
                                     const char **params, unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; i++) {
        const char *itr;
        if (i > 0) {
            if (string_append_char(buf, maxlen, plen, '&') != S3StatusOK)
                return S3StatusQueryParamsTooLong;
        }
        int hasValue = 0;
        for (itr = params[i]; *itr != '&' && *itr != '\0'; itr++) {
            if (*itr == '=') hasValue = 1;
            if (string_append_char(buf, maxlen, plen, *itr) != S3StatusOK)
                return S3StatusQueryParamsTooLong;
        }
        if (!hasValue) {
            if (string_append_char(buf, maxlen, plen, '=') != S3StatusOK)
                return S3StatusQueryParamsTooLong;
        }
    }
    return S3StatusOK;
}

// Simple comparison function for comparing two HTTP query parameters that are
// embedded within an HTTP URL, returning true if param1 comes
// before param2 alphabetically, false if not
static int paramle(const char *param1, const char *param2)
{
    while (1) {
        if (*param1 == '=' || *param1 == '&' || *param1 == '\0') {
            return 1;
        }
        else if (*param2 == '=' || *param2 == '&' || *param2 == '\0') {
            return 0;
        }
        else if (*param2 < *param1) {
            return 0;
        }
        else if (*param2 > *param1) {
            return 1;
        }
        param1++, param2++;
    }
}

/**
 * General gnome sort.
 */
typedef int (*less_func_t)(const char *p1, const char *p2);

static void general_gnome_sort(const char **params, int size,
                             less_func_t less)
{
    int i = 0, last_highest = 0;

    while (i < size) {
        if ((i == 0) || less(params[i - 1], params[i])) {
            i = ++last_highest;
        }
        else {
            const char *tmp = params[i];
            params[i] = params[i - 1];
            params[--i] = tmp;
        }
    }
}

static S3Status canonicalize_query_params(char *buf, int maxlen, int *plen,
                                       const char *querys)
{
    const char *query_params[1024];
    unsigned int count = 0;
    const char *itr;
    S3Status ret;

    query_params[count] = querys; count++;
    for (itr = querys; *itr != '\0'; itr++) {
        if (*itr == '&') {
            itr++;
            if (*itr == '\0' || *itr == '&' || *itr == '=')
                return S3StatusBadMetaData;
            if (count >= sizeof(query_params))
                return S3StatusQueryParamsTooLong;
            query_params[count] = itr;
            count++;
        }
    }
    general_gnome_sort(query_params, count, paramle);
    ret = string_append_params(buf, maxlen, plen, query_params, count);
    if (ret != S3StatusOK) return ret;
    ret = string_append_char(buf, maxlen, plen, '\n');
    return ret;
}

static S3Status canonicalize_uri(char *buf, int maxlen, int *plen,
                              const char *uri)
{
    const char *itr = uri;

#define check_and_skip(c) do{ if (*itr != (c)) return S3StatusErrorInvalidURI;\
        itr++; } while (0)
#define skip(c) do { if (*itr == (c)) itr++; } while(0)

    check_and_skip('h');check_and_skip('t');check_and_skip('t');check_and_skip('p');
    skip('s');check_and_skip(':');check_and_skip('/');check_and_skip('/');
    while (*itr != '/') itr++;
    for (;*itr != '\0' && *itr != '?'; itr++) {
        if (string_append_char(buf, maxlen, plen, *itr) != S3StatusOK)
            return S3StatusUriTooLong;
    }
    if (string_append_char(buf, maxlen, plen, '\n') != S3StatusOK)
        return S3StatusUriTooLong;
    if (*itr == '?') {
        return canonicalize_query_params(buf, maxlen, plen, ++itr);
    } else {
        // Empty query params, append a newline.
        if (string_append_char(buf, maxlen, plen, '\n') != S3StatusOK)
            return S3StatusUriTooLong;
    }
    return S3StatusOK;
}

static int headerle_nocase(const char *header1, const char *header2)
{
    while (1) {
        if (*header1 == ':') {
            return 1;
        }
        else if (*header2 == ':') {
            return 0;
        }
        else if (tolower(*header2) < tolower(*header1)) {
            return 0;
        }
        else if (tolower(*header2) > tolower(*header1)) {
            return 1;
        }
        header1++, header2++;
    }
}

static S3Status canonicalize_headers(char *buf, int maxlen, int *plen,
                                     struct curl_slist *curl_headers,
                                     RequestComputedValues *values)
{
    const char *headers[1024];
    unsigned int count = 0;
    char signedHeaders[4096];
    int signedHeadersLen = 0;
    struct curl_slist *itr;
    S3Status ret;

    // fix content length bug with element skip
    itr = curl_headers;
    if(itr && !strncasecmp(itr->data, "content-length", 15)){
        itr = curl_headers->next;
    }
    for (; itr != NULL; itr = itr->next) {
        if (count >= sizeof(headers)) return S3StatusHeadersTooLong;
        headers[count] = itr->data;
        count++;
    }

    general_gnome_sort(headers, count, headerle_nocase);
    // Now copy this sorted list into the buffer, all the while:
    // - folding repeated headers into single lines, and
    // - folding multiple lines
    // - removing the space after the colon
    unsigned int lastHeaderLen = 0, i;
    for (i = 0; i < count; i++) {
        const char *header = headers[i];
        const char *c = header;
        // If the header names are the same, append the next value
        if ((i > 0) && 
            !strncmp(header, headers[i - 1], lastHeaderLen)) {
            // Replacing the previous newline with a comma
            buf[*plen - 1] = ',';
            // Skip the header name and the ':'
            c += lastHeaderLen;
        } else {
            // Copy in everything up to the space in the ":"
            if (signedHeadersLen > 0) {
                ret = string_append_char(signedHeaders, sizeof(signedHeaders),
                                         &signedHeadersLen, ';');
                if (ret != S3StatusOK) return S3StatusHeadersTooLong;
            }
            for (; *c != ':'; c++) {
                char lowerc = tolower(*c);
                ret = string_append_char(buf, maxlen, plen, lowerc);
                if (ret != S3StatusOK) return S3StatusHeadersTooLong;
                ret = string_append_char(signedHeaders, sizeof(signedHeaders),
                                         &signedHeadersLen, lowerc);
                if (ret != S3StatusOK) return S3StatusHeadersTooLong;
            }
            // Save the header len since it's a new header
            c++;
            lastHeaderLen = c - header;
            ret = string_append_char(buf, maxlen, plen, ':');
            if (ret != S3StatusOK) return S3StatusHeadersTooLong;
        }
        // Trim leading whitespaces.
        while (is_blank(*c)) c++;
        // Now copy in the value, folding the lines
        while (*c) {
            // If c points to a \r\n[whitespace] sequence, then fold
            // this newline out
            if ((*c == '\r') && (*(c + 1) == '\n') && is_blank(*(c + 2))) {
                c += 3;
                while (is_blank(*c)) {
                    c++;
                }
                // Also, what has most recently been copied into buffer amy
                // have been whitespace, and since we're folding whitespace
                // out around this newline sequence, back buffer up over
                // any whitespace it contains
                while (is_blank(buf[*plen - 1])) {
                    --(*plen);
                }
                ret = string_append_char(buf, maxlen, plen, ',');
                if (ret != S3StatusOK) return S3StatusHeadersTooLong;
                continue;
            }
            ret = string_append_char(buf, maxlen, plen, *c);
            if (ret != S3StatusOK) return S3StatusHeadersTooLong;
            c++;
        }
        ret = string_append_char(buf, maxlen, plen, '\n');
        if (ret != S3StatusOK) return S3StatusHeadersTooLong;
    }
    // Append the newline after conanical headers.
    ret = string_append_char(buf, maxlen, plen, '\n');
    if (ret != S3StatusOK) return S3StatusHeadersTooLong;
    // Append the signed headers.
    for (i = 0; i < (unsigned int)signedHeadersLen; i++) {
        values->signedHeaders[i] = signedHeaders[i];
        ret = string_append_char(buf, maxlen, plen, signedHeaders[i]);
        if (ret != S3StatusOK) return S3StatusHeadersTooLong;
    }
    values->signedHeaders[signedHeadersLen] = '\0';
    ret = string_append_char(buf, maxlen, plen, '\n');
    if (ret != S3StatusOK) return S3StatusHeadersTooLong;
    return S3StatusOK;
}

static S3Status calculate_sha256(char *buf, int maxlen, int *plen,
                                 const char *data, int datalen)
{
    unsigned char hash256[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, data, datalen);
    SHA256_Final(hash256, &sha256);
    string_append_hex(buf, maxlen, plen, (const char *)hash256,
                      SHA256_DIGEST_LENGTH);
    return S3StatusOK;
}

S3Status calculate_hmac_sha256(char *buf, int maxlen,int *plen,
                                      const char *key, int keylen,
                                      const char *msg, int msglen)
{
    unsigned char *output = (unsigned char *)&buf[*plen];
    unsigned int md_len = 32;
    if (maxlen - *plen < 32) return S3StatusInternalError;
    HMAC(EVP_sha256(), key, keylen, (const unsigned char *)msg, msglen,
         output, &md_len);
    *plen += md_len;
    return S3StatusOK;
}

S3Status canonicalize_request_hash(char *buf, int maxlen, int *plen,
                                   Request *request,
                                   const RequestParams *params,
                                   RequestComputedValues *values)
{
    char canonicalRequest[20480];
    int len = 0;
    S3Status ret;

#define creq_append(format, ...)                             \
    len += snprintf(&(canonicalRequest[len]),                \
                    sizeof(canonicalRequest) - len,          \
                    format, __VA_ARGS__)

    creq_append("%s\n", http_request_type_to_verb(params->httpRequestType));
    ret = canonicalize_uri(canonicalRequest, sizeof(canonicalRequest), &len,
                           request->uri);
    if (ret != S3StatusOK) return ret;
    ret = canonicalize_headers(canonicalRequest, sizeof(canonicalRequest),
                               &len, request->headers, values);
    if (ret != S3StatusOK) return ret;
    if (params->putProperties && params->putProperties->md5
        && params->putProperties->md5[0]) {
        creq_append(
            "%s",params->putProperties->md5 + 1 + strlen(params->putProperties->md5)
        );
    } else {
        creq_append("%s", "UNSIGNED-PAYLOAD");
    }
    calculate_sha256(buf, maxlen, plen, canonicalRequest, len);
    return S3StatusOK;
}

#define S3_SERVICE "s3"

static S3Status canonicalize_scope(char *buf, int maxlen, int *plen,
                                   const RequestComputedValues *values)
{
    int i;
    for (i = 0; i < 8; i++) { // YYYYMMDD
        string_append_char(buf, maxlen, plen, values->timestamp[i]);
    }
    string_appendf(buf, maxlen, plen, "/%s/" S3_SERVICE "/aws4_request\n",
                   whichRegionIsHereG);
    return S3StatusOK;
}

#if 0
static void print_hash_inhex(const char *data)
{
    int i;
    for(i = 0; i < 32; i++) {
        printf("%02x", (unsigned char)data[i]);
    }
    printf("\n");
}
#endif

static S3Status compose_signing_key(char *buf, int maxlen, int *plen,
                                    const RequestParams *params,
                                    const RequestComputedValues *values)
{
    char secretKey[128];
    int keylen;
    char dateKey[32];
    char dateRegionKey[32];
    char dateRegionServiceKey[32];
    int hmaclen = 0;

    keylen = snprintf(secretKey, 128, "AWS4%s",
                      params->bucketContext.secretAccessKey);
    calculate_hmac_sha256(dateKey, 32, &hmaclen, secretKey, keylen,
                          values->timestamp, 8);
    hmaclen = 0;
    calculate_hmac_sha256(dateRegionKey, 32, &hmaclen, dateKey, 32,
                          whichRegionIsHereG, strlen(whichRegionIsHereG));
    hmaclen = 0;
    calculate_hmac_sha256(dateRegionServiceKey, 32, &hmaclen,
                          dateRegionKey, 32, S3_SERVICE, strlen(S3_SERVICE));
    calculate_hmac_sha256(buf, maxlen, plen, dateRegionServiceKey, 32,
                          "aws4_request", strlen("aws4_request"));
    return S3StatusOK;
}

S3Status compose_auth4_header(Request *request,
                              const RequestParams *params,
                              RequestComputedValues *values)
{
    char stringToSign[512];
    int stslen = 0;
    char signingKey[32];
    int sklen = 0;
    char signature[32];
    int siglen = 0;
    char authHeader[1024];
    int ahlen = 0;
    int i;

    string_appendf(stringToSign, sizeof(stringToSign), &stslen,
                   "AWS4-HMAC-SHA256\n%s\n", values->timestamp);
    canonicalize_scope(stringToSign, sizeof(stringToSign), &stslen, values);
    canonicalize_request_hash(stringToSign, sizeof(stringToSign), &stslen,
                              request, params, values);
    compose_signing_key(signingKey, sizeof(signingKey), &sklen, params,values);
    calculate_hmac_sha256(signature, sizeof(signature), &siglen,
                          signingKey, sizeof(signingKey),
                          stringToSign, stslen);
    string_appendf(authHeader, sizeof(authHeader), &ahlen,
                   "Authorization: AWS4-HMAC-SHA256 Credential=%s/",
                   params->bucketContext.accessKeyId);
    for (i = 0; i < 8; i++) {
        string_append_char(authHeader, sizeof(authHeader), &ahlen,
                           values->timestamp[i]);
    }
    string_appendf(authHeader, sizeof(authHeader), &ahlen,
                   "/%s/%s/aws4_request, SignedHeaders=%s, Signature=",
                   whichRegionIsHereG, S3_SERVICE, values->signedHeaders);
    string_append_hex(authHeader, sizeof(authHeader), &ahlen,
                      signature, sizeof(signature));
    curl_slist_append(request->headers, authHeader);
    return S3StatusOK;
}

/**
 *
 * @return 1 if match found, 0 if no SubjectAlternativeNames present, -1 if no match found
 */
static int matches_subject_alt_name(const char * hostname, const X509 * server_cert)
{
    int i, hostname_len;
    int san_names_count = -1;
    int result = -1;
    STACK_OF(GENERAL_NAME) *san_names = NULL;

    san_names = X509_get_ext_d2i((X509 *)server_cert, NID_subject_alt_name, NULL, NULL);
    if (san_names == NULL) {
        return 0;
    }

    san_names_count = sk_GENERAL_NAME_num(san_names);

    if (san_names_count > 0) {
        hostname_len = strlen(hostname);

        for (i = 0; i < san_names_count; i++) {
            const GENERAL_NAME * current_name = sk_GENERAL_NAME_value(san_names, i);

            if (current_name->type == GEN_DNS) {
                char * dns_name = (char *)ASN1_STRING_data(current_name->d.dNSName);
                int dns_len = strlen(dns_name);

                // Make sure there isn't an embedded NUL character in the DNS name
                if (ASN1_STRING_length(current_name->d.dNSName) != dns_len) {
                    // if there is a NUL, assume it's malicious or an accident and ignore it 
                    fprintf(stderr, "Embedded NUL char in DNS name %s with specified length of %d, skipping this entry\n", dns_name, dns_len);
                } else if (hostname_len == dns_len && strncasecmp(hostname, dns_name, hostname_len) == 0){
                    // exact match (case-insensitive)
                    result = 1;
                    break;
                } else if (dns_len > 2 && dns_name[0] == '*' && dns_name[1] == '.') {
                    // simple single-wildcard matching
                    char * dns_suffix = dns_name + 1;
                    char * target_suffix = strchr(hostname, '.');
                    if (target_suffix && strcasecmp(target_suffix, dns_suffix) == 0) {
                        result = 1;
                        break;
                    }
                }
            }
        }
    }

    sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);

    return result;
}

static int matches_common_name(const char * hostname, const X509 * server_cert)
{
    int common_name_loc = -1;
    X509_NAME_ENTRY *common_name_entry = NULL;
    ASN1_STRING *common_name_asn1 = NULL;
    char *common_name_str = NULL;
    size_t common_name_strlen = 0;

    // Find the position of the CN field in the Subject field of the certificate
    common_name_loc = X509_NAME_get_index_by_NID(X509_get_subject_name((X509 *)server_cert), NID_commonName, -1);
    if (common_name_loc < 0) {
        //fprintf(stderr, "Could not find CN in server cert\n");
        return -1;
    }

    // Extract the CN field
    common_name_entry = X509_NAME_get_entry(X509_get_subject_name((X509 *)server_cert), common_name_loc);
    if (common_name_entry == NULL) {
        //fprintf(stderr, "Failed to get CN from server cert\n");
        return -1;
    }

    // Convert the CN field to a C string
    common_name_asn1 = X509_NAME_ENTRY_get_data(common_name_entry);
    if (common_name_asn1 == NULL) {
        //fprintf(stderr, "Failed to get CN data from server cert\n");
        return -1;
    }
    common_name_str = (char *) ASN1_STRING_data(common_name_asn1);
    common_name_strlen = strlen(common_name_str);

    // Make sure there isn't an embedded NUL character in the CN
    if ((size_t)ASN1_STRING_length(common_name_asn1) != common_name_strlen) {
        //fprintf(stderr, "CN data from server cert malformed\n");
        return -1;
    }

    // Compare expected hostname with the CN
    if (strcasecmp(hostname, common_name_str) == 0) {
        return 1;
    } else {
        //fprintf(stderr, "CN in server cert '%s' doesn't match '%s'\n", common_name_str, hostname);
        return -1;
    }
} 

static int ssl_verify_callback(int preverify_ok, X509_STORE_CTX *x509_context) 
{ 
    SSL * ssl;
    int depth = X509_STORE_CTX_get_error_depth(x509_context);
   
    if (!preverify_ok) {
        // something already failed, so just return
        return preverify_ok;
    }

    if (depth != 0) {
        // we're only doing work on the leaf cert (at depth 0)
        return preverify_ok;
    }

    X509 * peer_cert = X509_STORE_CTX_get_current_cert(x509_context);
    ssl = X509_STORE_CTX_get_ex_data(x509_context, SSL_get_ex_data_X509_STORE_CTX_idx());
    SSL_CTX * ctx = SSL_get_SSL_CTX(ssl);
    const char * host = SSL_CTX_get_app_data(ctx);

    int match = matches_subject_alt_name(host, peer_cert);

    if (match == 0) {
        // no SAN!
        match = matches_common_name(host, peer_cert);
    }

    if (match != 1) {
        fprintf(stderr, "expected hostname '%s' doesn't match up with presented cert\n", host);
        X509_STORE_CTX_set_error(x509_context, X509_V_ERR_APPLICATION_VERIFICATION); 
        return 0;
    }

    X509_STORE_CTX_set_error(x509_context, X509_V_OK); 
    return 1;
} 

static CURLcode ssl_context_callback(CURL *handle, SSL_CTX *context, void *data) 
{ 
    //SSL_CTX_set_cert_verify_callback(context, &ssl_verify_callback, data); 
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, &ssl_verify_callback);
    SSL_CTX_set_app_data(context, data);
    return CURLE_OK;
} 

// Sets up the curl handle given the completely computed RequestParams
static S3Status setup_curl(Request *request,
                           const RequestParams *params,
                           RequestComputedValues *values)
{
    CURLcode status;

#define curl_easy_setopt_safe(opt, val)                                 \
    if ((status = curl_easy_setopt                                      \
         (request->curl, opt, val)) != CURLE_OK) {                      \
        return S3StatusFailedToInitializeRequest;                       \
    }

    // Debugging only
    if (params->bucketContext.curlVerboseLogging) {
        curl_easy_setopt_safe(CURLOPT_VERBOSE, 1);
    }
    
    // Set private data to request for the benefit of S3RequestContext
    curl_easy_setopt_safe(CURLOPT_PRIVATE, request);
    
    // Set header callback and data
    curl_easy_setopt_safe(CURLOPT_HEADERDATA, request);
    curl_easy_setopt_safe(CURLOPT_HEADERFUNCTION, &curl_header_func);
    
    // Set read callback, data, and readSize
    curl_easy_setopt_safe(CURLOPT_READFUNCTION, &curl_read_func);
    curl_easy_setopt_safe(CURLOPT_READDATA, request);
    
    // Set write callback and data
    curl_easy_setopt_safe(CURLOPT_WRITEFUNCTION, &curl_write_func);
    curl_easy_setopt_safe(CURLOPT_WRITEDATA, request);

    // Ask curl to parse the Last-Modified header.  This is easier than
    // parsing it ourselves.
    curl_easy_setopt_safe(CURLOPT_FILETIME, 1);

    // Curl docs suggest that this is necessary for multithreaded code.
    // However, it also points out that DNS timeouts will not be honored
    // during DNS lookup, which can be worked around by using the c-ares
    // library, which we do not do yet.
    curl_easy_setopt_safe(CURLOPT_NOSIGNAL, 1);

    // Turn off Curl's built-in progress meter
    curl_easy_setopt_safe(CURLOPT_NOPROGRESS, 1);

    // xxx todo - support setting the proxy for Curl to use (can't use https
    // for proxies though)

    // xxx todo - support setting the network interface for Curl to use

    // I think this is useful - we don't need interactive performance, we need
    // to complete large operations quickly
    curl_easy_setopt_safe(CURLOPT_TCP_NODELAY, 1);
    
    // Don't use Curl's 'netrc' feature
    curl_easy_setopt_safe(CURLOPT_NETRC, CURL_NETRC_IGNORED);

    // Don't verify S3's certificate unless S3_INIT_VERIFY_PEER is set.
    // The request_context may be set to override this
    curl_easy_setopt_safe(CURLOPT_SSL_VERIFYPEER, verifyPeer);

    if (caInfoG[0]) {
        curl_easy_setopt_safe(CURLOPT_CAINFO, caInfoG);
    }

    // disable usage of TLS session id's because this breaks stuff sometimes.
    // from curl docs: https://curl.se/libcurl/c/CURLOPT_SSL_SESSIONID_CACHE.html
    // While nothing ever should get hurt by attempting to reuse SSL session-IDs,
    // there seem to be or have been broken SSL implementations in the wild that
    // may require you to disable this in order for you to succeed.
    curl_easy_setopt_safe(CURLOPT_SSL_SESSIONID_CACHE, 0L);

    // always use TLSv1.2 or higher, whether we allow higher is configurable
    if (params->bucketContext.unboundTlsVersion) {
        curl_easy_setopt_safe(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        if (params->bucketContext.curlVerboseLogging) {
            fprintf(stderr, "Specified TLS 1.2+ for cURL\n");
        }
    } else {
        curl_easy_setopt_safe(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2);
        if (params->bucketContext.curlVerboseLogging) {
            fprintf(stderr, "Specified TLS 1.2 for cURL\n");
        }
    }
    
    if (params->bucketContext.curlConnectToFullySpecified && params->bucketContext.curlConnectToFullySpecified[0]) {
        request->connect_to_list = curl_slist_append(NULL, params->bucketContext.curlConnectToFullySpecified);
        curl_easy_setopt_safe(CURLOPT_CONNECT_TO, request->connect_to_list);
        if (params->bucketContext.curlVerboseLogging) {
            fprintf(stderr, "CURLOPT_CONNECT_TO=%s\n", params->bucketContext.curlConnectToFullySpecified);
        }
    }

    if (verifyPeer && params->bucketContext.hostHeaderValue && params->bucketContext.hostHeaderValue[0]) {
        // we're installing our own hostname verification here
        // turn OFF default hostname verification
        curl_easy_setopt_safe(CURLOPT_SSL_VERIFYHOST, 0); 
        // install our own hostname verification
        curl_easy_setopt_safe(CURLOPT_SSL_CTX_FUNCTION, (curl_ssl_ctx_callback)&ssl_context_callback); 
        curl_easy_setopt_safe(CURLOPT_SSL_CTX_DATA, params->bucketContext.hostHeaderValue);
    }
    
    // Follow any redirection directives that S3 sends
    curl_easy_setopt_safe(CURLOPT_FOLLOWLOCATION, 1);

    // A safety valve in case S3 goes bananas with redirects
    curl_easy_setopt_safe(CURLOPT_MAXREDIRS, 10);

    // Set the User-Agent; maybe Amazon will track these?
    curl_easy_setopt_safe(CURLOPT_USERAGENT, userAgentG);

    // Set the low speed limit and time; we abort transfers that stay at
    // less than 1K per second for more than 15 seconds.
    // xxx todo - make these configurable
    // xxx todo - allow configurable max send and receive speed
    curl_easy_setopt_safe(CURLOPT_LOW_SPEED_LIMIT, 1024);
    curl_easy_setopt_safe(CURLOPT_LOW_SPEED_TIME, 15);

    // Append standard headers
#define append_standard_header(fieldName)                               \
    if (values-> fieldName [0]) {                                       \
        request->headers = curl_slist_append(request->headers,          \
                                             values-> fieldName);       \
    }

    // Would use CURLOPT_INFILESIZE_LARGE, but it is buggy in libcurl
    if ((params->httpRequestType == HttpRequestTypePUT) ||
        (params->httpRequestType == HttpRequestTypePOST)) {
        curl_easy_setopt_safe(
            CURLOPT_INFILESIZE,
            (unsigned long long) params->toS3CallbackTotalSize
        );
        char header[256];
        snprintf(header, sizeof(header), "Content-Length: %llu",
                 (unsigned long long) params->toS3CallbackTotalSize);
        request->headers = curl_slist_append(request->headers, header);
        //request->headers = curl_slist_append(request->headers,
        //                                     "Transfer-Encoding:");
    }
    else if (params->httpRequestType == HttpRequestTypeCOPY) {
        request->headers = curl_slist_append(request->headers, 
                                             "Transfer-Encoding:");
    }
    
    append_standard_header(hostHeader);
    append_standard_header(cacheControlHeader);
    append_standard_header(contentTypeHeader);
    append_standard_header(md5Header);
    append_standard_header(contentDispositionHeader);
    append_standard_header(contentEncodingHeader);
    append_standard_header(expiresHeader);
    append_standard_header(ifModifiedSinceHeader);
    append_standard_header(ifUnmodifiedSinceHeader);
    append_standard_header(ifMatchHeader);
    append_standard_header(ifNoneMatchHeader);
    append_standard_header(rangeHeader);
    if (signatureVersionG == S3SignatureV2) {
        append_standard_header(authorizationHeader);
    }

    // Append x-amz- headers
    int i;
    for (i = 0; i < values->amzHeadersCount; i++) {
        request->headers = 
            curl_slist_append(request->headers, values->amzHeaders[i]);
    }

    // Append signature version 4 if needed.
    if (signatureVersionG == S3SignatureV4) {
        compose_auth4_header(request, params, values);
    }

    // Set the HTTP headers
    curl_easy_setopt_safe(CURLOPT_HTTPHEADER, request->headers);

    // Set URI
    curl_easy_setopt_safe(CURLOPT_URL, request->uri);

    // Set request type.
    switch (params->httpRequestType) {
    case HttpRequestTypeHEAD:
        curl_easy_setopt_safe(CURLOPT_NOBODY, 1);
        break;
    case HttpRequestTypePOST:
        curl_easy_setopt_safe(CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt_safe(CURLOPT_UPLOAD, 1);
        break;

    case HttpRequestTypePUT:
    case HttpRequestTypeCOPY:
        curl_easy_setopt_safe(CURLOPT_UPLOAD, 1);
        break;
    case HttpRequestTypeDELETE:
        curl_easy_setopt_safe(CURLOPT_CUSTOMREQUEST, "DELETE");
        break;
    default: // HttpRequestTypeGET
        break;
    }
    
    return S3StatusOK;
}


static void request_deinitialize(Request *request)
{
    if (request->headers) {
        curl_slist_free_all(request->headers);
    }

    if (request->connect_to_list) {
        curl_slist_free_all(request->connect_to_list);
    }
    
    error_parser_deinitialize(&(request->errorParser));

    // curl_easy_reset prevents connections from being re-used for some
    // reason.  This makes HTTP Keep-Alive meaningless and is very bad for
    // performance.  But it is necessary to allow curl to work properly.
    // xxx todo figure out why
    curl_easy_reset(request->curl);
}


static S3Status request_get(const RequestParams *params, 
                            RequestComputedValues *values,
                            Request **reqReturn)
{
    Request *request = 0;
    
    // Try to get one from the request stack.  We hold the lock for the
    // shortest time possible here.
    pthread_mutex_lock(&requestStackMutexG);

    if (requestStackCountG) {
        request = requestStackG[--requestStackCountG];
    }
    
    pthread_mutex_unlock(&requestStackMutexG);

    // If we got one, deinitialize it for re-use
    if (request) {
        request_deinitialize(request);
    }
    // Else there wasn't one available in the request stack, so create one
    else {
        if (!(request = (Request *) malloc(sizeof(Request)))) {
            return S3StatusOutOfMemory;
        }
        if (!(request->curl = curl_easy_init())) {
            free(request);
            return S3StatusFailedToInitializeRequest;
        }
    }

    // Initialize the request
    request->prev = 0;
    request->next = 0;

    // Request status is initialized to no error, will be updated whenever
    // an error occurs
    request->status = S3StatusOK;

    S3Status status;
                        
    // Start out with no headers
    request->headers = 0;

    // Start out with no connect-to
    request->connect_to_list = 0;

    // Compute the URL
    if ((status = compose_uri
         (request->uri, sizeof(request->uri), 
          &(params->bucketContext), values->urlEncodedKey,
          params->subResource, params->queryParams)) != S3StatusOK) {
        curl_easy_cleanup(request->curl);
        free(request);
        return status;
    }

    // Set all of the curl handle options
    if ((status = setup_curl(request, params, values)) != S3StatusOK) {
        curl_easy_cleanup(request->curl);
        free(request);
        return status;
    }

    request->propertiesCallback = params->propertiesCallback;

    request->toS3Callback = params->toS3Callback;

    request->toS3CallbackBytesRemaining = params->toS3CallbackTotalSize;

    request->fromS3Callback = params->fromS3Callback;

    request->completeCallback = params->completeCallback;

    request->callbackData = params->callbackData;

    response_headers_handler_initialize(&(request->responseHeadersHandler));

    request->propertiesCallbackMade = 0;
    
    error_parser_initialize(&(request->errorParser));

    *reqReturn = request;
    
    return S3StatusOK;
}


static void request_destroy(Request *request)
{
    request_deinitialize(request);
    curl_easy_cleanup(request->curl);
    free(request);
}


static void request_release(Request *request)
{
    pthread_mutex_lock(&requestStackMutexG);

    // If the request stack is full, destroy this one
    if (requestStackCountG == REQUEST_STACK_SIZE) {
        pthread_mutex_unlock(&requestStackMutexG);
        request_destroy(request);
    }
    // Else put this one at the front of the request stack; we do this because
    // we want the most-recently-used curl handle to be re-used on the next
    // request, to maximize our chances of re-using a TCP connection before it
    // times out
    else {
        requestStackG[requestStackCountG++] = request;
        pthread_mutex_unlock(&requestStackMutexG);
    }
}

S3Status S3_set_region_name(const char *regionName)
{
    if (regionName != NULL) {
        if (snprintf(whichRegionIsHereG, sizeof(whichRegionIsHereG),
                     "%s", regionName) >= S3_MAX_HOSTNAME_SIZE) {
            return S3StatusUriTooLong;
        }
    }
    return S3StatusOK;
}

S3Status S3_set_ca_info(const char *caInfo)
{
    if (caInfo != NULL) {
        if (snprintf(caInfoG, sizeof(caInfoG),
                     "%s", caInfo) >= S3_MAX_HOSTNAME_SIZE) {
            return S3StatusUriTooLong;
        }
    }
    return S3StatusOK;
}

S3Status request_api_initialize(const char *userAgentInfo, int flags,
                                const char *defaultHostName)
{
    if (curl_global_init(CURL_GLOBAL_ALL & 
                         ~((flags & S3_INIT_WINSOCK) ? 0 : CURL_GLOBAL_WIN32))
        != CURLE_OK) {
        return S3StatusInternalError;
    }
    verifyPeer = (flags & S3_INIT_VERIFY_PEER) != 0;

    if (flags & S3_INIT_SIGNATURE_V4) {
        signatureVersionG = S3SignatureV4;
    } else {
        signatureVersionG = S3SignatureV2;
    }

    if (!defaultHostName) {
        defaultHostName = S3_DEFAULT_HOSTNAME;
    }

    if (snprintf(defaultHostNameG, S3_MAX_HOSTNAME_SIZE, 
                 "%s", defaultHostName) >= S3_MAX_HOSTNAME_SIZE) {
        return S3StatusUriTooLong;
    }

    pthread_mutex_init(&requestStackMutexG, 0);

    requestStackCountG = 0;

    if (!userAgentInfo || !*userAgentInfo) {
        userAgentInfo = "Unknown";
    }

    char platform[96];
    struct utsname utsn;
    if (uname(&utsn)) {
        snprintf(platform, sizeof(platform), "Unknown");
    }
    else {
        snprintf(platform, sizeof(platform), "%s%s%s", utsn.sysname, 
                 utsn.machine[0] ? " " : "", utsn.machine);
    }

    snprintf(userAgentG, sizeof(userAgentG), 
             "Mozilla/4.0 (Compatible; %s; libs3 %s.%s; %s)",
             userAgentInfo, LIBS3_VER_MAJOR, LIBS3_VER_MINOR, platform);
    
    return S3StatusOK;
}


void request_api_deinitialize()
{
    pthread_mutex_destroy(&requestStackMutexG);

    while (requestStackCountG--) {
        request_destroy(requestStackG[requestStackCountG]);
    }
}

void request_perform(const RequestParams *params, S3RequestContext *context)
{
    Request *request;
    S3Status status;
    int verifyPeerRequest = verifyPeer;
    CURLcode curlstatus;

#define return_status(status)                                           \
    (*(params->completeCallback))(status, 0, params->callbackData);     \
    return

    // These will hold the computed values
    RequestComputedValues computed;

    // Validate the bucket name
    if (params->bucketContext.bucketName && 
        ((status = S3_validate_bucket_name
          (params->bucketContext.bucketName, 
           params->bucketContext.uriStyle)) != S3StatusOK)) {
        return_status(status);
    }

    // Compose the amz headers
    if ((status = compose_amz_headers(params, &computed)) != S3StatusOK) {
        return_status(status);
    }

    // Compose standard headers
    if ((status = compose_standard_headers
         (params, &computed)) != S3StatusOK) {
        return_status(status);
    }

    // URL encode the key
    if ((status = encode_key(params, &computed)) != S3StatusOK) {
        return_status(status);
    }

    if (signatureVersionG == S3SignatureV2) {
        // Compute the canonicalized amz headers
        canonicalize_amz_headers(&computed);

        // Compute the canonicalized resource
        canonicalize_resource(params->bucketContext.bucketName,
                              params->subResource, computed.urlEncodedKey,
                              computed.canonicalizedResource);

        // Compose Authorization header
        if ((status = compose_auth_header(params, &computed)) != S3StatusOK) {
            return_status(status);
        }
    }
    
    // Get an initialized Request structure now
    if ((status = request_get(params, &computed, &request)) != S3StatusOK) {
        return_status(status);
    }
    if (context && context->verifyPeerSet) {
        verifyPeerRequest = context->verifyPeerSet;
    }
    // Allow per-context override of verifyPeer
    if (verifyPeerRequest != verifyPeer) {
            if ((curlstatus = curl_easy_setopt(request->curl, 
                                               CURLOPT_SSL_VERIFYPEER, 
                                               context->verifyPeer))
                != CURLE_OK) {
                return_status(S3StatusFailedToInitializeRequest);
            }
    }

    // If a RequestContext was provided, add the request to the curl multi
    if (context) {
        CURLMcode code = curl_multi_add_handle(context->curlm, request->curl);
        if (code == CURLM_OK) {
            if (context->requests) {
                request->prev = context->requests->prev;
                request->next = context->requests;
                context->requests->prev->next = request;
                context->requests->prev = request;
            }
            else {                
                context->requests = request->next = request->prev = request;
            }
        }
        else {
            fprintf(stderr, "request_perform.curl_multi_add_handle failed: %s\n", curl_easy_strerror(code));
            if (request->status == S3StatusOK) {
                request->status = (code == CURLM_OUT_OF_MEMORY) ?
                    S3StatusOutOfMemory : S3StatusInternalError;
            }
            request_finish(request);
        }
    }
    // Else, perform the request immediately
    else {
        CURLcode code = curl_easy_perform(request->curl);
        if ((code != CURLE_OK) && (request->status == S3StatusOK)) {
            fprintf(stderr, "request_perform.curl_easy_perform failed: %s\n", curl_easy_strerror(code));
            request->status = request_curl_code_to_status(code);
        }

        // Finish the request, ensuring that all callbacks have been made, and
        // also releases the request
        request_finish(request);
    }
}


void request_finish(Request *request)
{
    // If we haven't detected this already, we now know that the headers are
    // definitely done being read in
    request_headers_done(request);
    
    // If there was no error processing the request, then possibly there was
    // an S3 error parsed, which should be converted into the request status
    if (request->status == S3StatusOK) {
        error_parser_convert_status(&(request->errorParser), 
                                    &(request->status));
        // If there still was no error recorded, then it is possible that
        // there was in fact an error but that there was no error XML
        // detailing the error
        if ((request->status == S3StatusOK) &&
            ((request->httpResponseCode < 200) ||
             (request->httpResponseCode > 299))) {
            switch (request->httpResponseCode) {
            case 0:
                // This happens if the request never got any HTTP response
                // headers at all, we call this a ConnectionFailed error
                request->status = S3StatusConnectionFailed;
                break;
            case 100: // Some versions of libcurl erroneously set HTTP
                      // status to this
                break;
            case 301:
                request->status = S3StatusErrorPermanentRedirect;
                break;
            case 307:
                request->status = S3StatusHttpErrorMovedTemporarily;
                break;
            case 400:
                request->status = S3StatusHttpErrorBadRequest;
                break;
            case 403: 
                request->status = S3StatusHttpErrorForbidden;
                break;
            case 404:
                request->status = S3StatusHttpErrorNotFound;
                break;
            case 405:
                request->status = S3StatusErrorMethodNotAllowed;
                break;
            case 409:
                request->status = S3StatusHttpErrorConflict;
                break;
            case 411:
                request->status = S3StatusErrorMissingContentLength;
                break;
            case 412:
                request->status = S3StatusErrorPreconditionFailed;
                break;
            case 416:
                request->status = S3StatusErrorInvalidRange;
                break;
            case 500:
                request->status = S3StatusErrorInternalError;
                break;
            case 501:
                request->status = S3StatusErrorNotImplemented;
                break;
            case 503:
                request->status = S3StatusErrorSlowDown;
                break;
            default:
                request->status = S3StatusHttpErrorUnknown;
                break;
            }
        }
    }

    (*(request->completeCallback))
        (request->status, &(request->errorParser.s3ErrorDetails),
         request->callbackData);

    request_release(request);
}


S3Status request_curl_code_to_status(CURLcode code)
{
    switch (code) {
    case CURLE_OUT_OF_MEMORY:
        return S3StatusOutOfMemory;
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
        return S3StatusNameLookupError;
    case CURLE_COULDNT_CONNECT:
        return S3StatusFailedToConnect;
    case CURLE_WRITE_ERROR:
    case CURLE_OPERATION_TIMEDOUT:
        return S3StatusConnectionFailed;
    case CURLE_PARTIAL_FILE:
        return S3StatusOK;
    case CURLE_PEER_FAILED_VERIFICATION:
        return S3StatusServerFailedVerification;
    default:
        return S3StatusInternalError;
    }
}


S3Status S3_generate_authenticated_query_string
    (char *buffer, const S3BucketContext *bucketContext,
     const char *key, int64_t expires, const char *resource)
{
#define MAX_EXPIRES (((int64_t) 1 << 31) - 1)
    // S3 seems to only accept expiration dates up to the number of seconds
    // representably by a signed 32-bit integer
    if (expires < 0) {
        expires = MAX_EXPIRES;
    }
    else if (expires > MAX_EXPIRES) {
        expires = MAX_EXPIRES;
    }

    // xxx todo: rework this so that it can be incorporated into shared code
    // with request_perform().  It's really unfortunate that this code is not
    // shared with request_perform().

    // URL encode the key
    char urlEncodedKey[S3_MAX_KEY_SIZE * 3];
    if (key) {
        urlEncode(urlEncodedKey, key, strlen(key));
    }
    else {
        urlEncodedKey[0] = 0;
    }

    // Compute canonicalized resource
    char canonicalizedResource[MAX_CANONICALIZED_RESOURCE_SIZE];
    canonicalize_resource(bucketContext->bucketName, resource, urlEncodedKey,
                          canonicalizedResource);
                          
    // We allow for:
    // 17 bytes for HTTP-Verb + \n
    // 1 byte for empty Content-MD5 + \n
    // 1 byte for empty Content-Type + \n
    // 20 bytes for Expires + \n
    // 0 bytes for CanonicalizedAmzHeaders
    // CanonicalizedResource
    char signbuf[17 + 1 + 1 + 1 + 20 + sizeof(canonicalizedResource) + 1];
    int len = 0;

#define signbuf_append(format, ...)                             \
    len += snprintf(&(signbuf[len]), sizeof(signbuf) - len,     \
                    format, __VA_ARGS__)

    signbuf_append("%s\n", "GET"); // HTTP-Verb
    signbuf_append("%s\n", ""); // Content-MD5
    signbuf_append("%s\n", ""); // Content-Type
    signbuf_append("%llu\n", (unsigned long long) expires);
    signbuf_append("%s", canonicalizedResource);

    // Generate an HMAC-SHA-1 of the signbuf
    unsigned char hmac[20];

    HMAC_SHA1(hmac, (unsigned char *) bucketContext->secretAccessKey,
              strlen(bucketContext->secretAccessKey),
              (unsigned char *) signbuf, len);

    // Now base-64 encode the results
    char b64[((20 + 1) * 4) / 3];
    int b64Len = base64Encode(hmac, 20, b64);

    // Now urlEncode that
    char signature[sizeof(b64) * 3];
    urlEncode(signature, b64, b64Len);

    // Finally, compose the uri, with params:
    // ?AWSAccessKeyId=xxx[&Expires=]&Signature=xxx
    char queryParams[sizeof("AWSAccessKeyId=") + 20 + 
                     sizeof("&Expires=") + 20 + 
                     sizeof("&Signature=") + sizeof(signature) + 1];

    sprintf(queryParams, "AWSAccessKeyId=%s&Expires=%ld&Signature=%s",
            bucketContext->accessKeyId, (long) expires, signature);

    return compose_uri(buffer, S3_MAX_AUTHENTICATED_QUERY_STRING_SIZE,
                       bucketContext, urlEncodedKey, resource, queryParams);
}

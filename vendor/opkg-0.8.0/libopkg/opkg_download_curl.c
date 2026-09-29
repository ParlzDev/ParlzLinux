/* vi: set expandtab sw=4 sts=4: */
/* opkg_download.c - the opkg package management system

   Copyright (C) 2001 University of Southern California
   Copyright (C) 2008 OpenMoko Inc
   Copyright (C) 2014 Paul Barker

   SPDX-License-Identifier: GPL-2.0-or-later

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#include "config.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "opkg_download.h"
#include "opkg_message.h"
#include "opkg_utils.h"

#include "sprintf_alloc.h"
#include "file_util.h"
#include "xfuncs.h"


/*
 * Make curl and the error buffer instance variables so we don't have to
 * instantiate them each time.
 */
static CURL *curl = NULL;
static char *curl_errorbuffer = NULL;
static CURL *opkg_curl_init(curl_progress_func cb, void *data);

static size_t dummy_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;

    return size * nmemb;
}

/** \brief header_write: curl callback that extracts HTTP ETag header
 *
 * \param ptr complete HTTP header line
 * \param size size of each data element
 * \param nmemb number of data elements
 * \param userdata pointer to data for storing ETag header value
 * \return number of processed bytes
 *
 */
static size_t header_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    char prefix[5];
    unsigned long i;
    for (i = 0; (i < 5) && (i < size * nmemb); ++i)
        prefix[i] = tolower(ptr[i]);
    if (str_starts_with(prefix, "etag:")) {
        char **out = userdata;
        char *start = strchr(ptr, '"') + 1;
        char *end = strrchr(ptr, '"');
        if (end > start)
            *out = strndup(start, end - start);
    }
    return size * nmemb;
}

/**
 * \brief Log an error message in case a download has failed.
 *
 * The error message consists of three parts: The passed @c msg, followed by the
 * @c src_url and information from libcurl about the failure.
 *
 * For the last part, libcurl's ERRORBUFFER (see [0]) is used if it is
 * non-empty. If it is, curl_easy_strerror() is used instead as a fallback -
 * which will result in a more generic description of the error (e.g. 'HTTP
 * response code said error' instead of sth. like 'The requested URL returned
 * error: 401').
 *
 * [0]: https://curl.se/libcurl/c/CURLOPT_ERRORBUFFER.html
 */
static void log_curl_download_error(const char *msg, const char *src_url, CURLcode res)
{
    const size_t curl_err_len = strlen(curl_errorbuffer);
    const char *curl_err_msg =
        (curl_err_len) ? curl_errorbuffer : curl_easy_strerror(res);
    // According to the example code in the documentation, messages returned by
    // curl_easy_strerror() will never have a trailing newline, while those in
    // the error buffer might.
    const int has_trailing_newline =
        curl_err_len && curl_err_msg[curl_err_len - 1] == '\n';
    opkg_msg(ERROR, "%s %s: %s%s",
             msg,
             src_url,
             curl_err_msg,
             (has_trailing_newline) ? "" : "\n");
}

/*
 * \brief Wrapper for curl_easy_perform() that resets the error buffer to an
 * empty string first.
 *
 * According to the documentation, there is no need to initialize the buffer for
 * libcurl versions 7.60.0 and higher. Since we don't require a specific
 * version, we simply always reset it to be on the safe side.
 */
CURLcode perform_curl_request(CURL *handle)
{
    curl_errorbuffer[0] = 0;
    return curl_easy_perform(handle);
}

#ifdef HAVE_SSLCURL
/*
 * Creates a newly allocated string with the same content as str, but
 * the first occurence of "token" is replaced with "replacement".
 * Returns a pointer to the newly created string's first byte.
 * If the substring "token" is not present in the string "str",
 * or if token is the empty string, it returns a newly allocated string
 * that is identical to "str".
 */
static char *replace_token_in_str(const char *str, const char *token,
                                  const char *replacement)
{
    /*
     * There's nothing to replace, just clone the string to be consistent with
     * the fact that the user gets a newly allocated string back
     */
    if (!token || *token == '\0')
        return xstrdup(str);

    char *found_token = strstr(str, token);
    if (!found_token)
        /*
         * There's nothing to replace, just clone the string to be consistent
         * with the fact that the user gets a newly allocated string back
         */
        return xstrdup(str);

    size_t replacement_len = strlen(replacement);
    size_t str_len = strlen(str);
    size_t token_len = strlen(token);

    size_t replaced_str_len = str_len - (token_len - replacement_len);

    unsigned int token_idx = found_token - str;
    char *replaced_str = xmalloc((replaced_str_len + 1) * sizeof(char));

    /* first copy the string part *before* the substring to replace */
    memmove(replaced_str, str, token_idx);
    /* then copy the replacement to the same position than the token */
    memmove(replaced_str + token_idx, replacement, replacement_len);
    /* finally complete the string with characters following the token */
    memmove(replaced_str + token_idx + replacement_len,
            str + token_idx + token_len,
            (replaced_str_len - token_idx - replacement_len));

    replaced_str[replaced_str_len] = '\0';

    return replaced_str;
}
#endif                          /* HAVE_SSLCURL */

/** \brief create_file_stamp: creates stamp for file
 *
 * \param file_name absolute file name
 * \param stamp stamp data for file
 * \return 0 if success, -1 if error occurs
 *
 */
static int create_file_stamp(const char *file_name, char *stamp)
{
    FILE *file;
    char *file_path;

    sprintf_alloc(&file_path, "%s.@stamp", file_name);
    file = fopen(file_path, "wb");
    if (file == NULL) {
        opkg_msg(ERROR, "Failed to open file %s\n", file_path);
        free(file_path);
        return -1;
    }
    fwrite(stamp, strlen(stamp), 1, file);
    fclose(file);
    free(file_path);
    return 0;
}

#define STAMP_BUF_SIZE 10
/** \brief check_file_stamp: compares provided stamp with existing file stamp
 *
 * \param file_name absolute file name
 * \param stamp stamp data to compare with existing file stamp
 * \return 0 if both stamps are equal or -1 otherwise
 *
 */
static int check_file_stamp(const char *file_name, char *stamp)
{
    FILE *file;
    char stamp_buf[STAMP_BUF_SIZE];
    char *file_path;
    int size;
    int diff = 0;
    int r;

    sprintf_alloc(&file_path, "%s.@stamp", file_name);
    if (!file_exists(file_path)) {
        free(file_path);
        return -1;
    }
    file = fopen(file_path, "rb");
    if (file == NULL) {
        opkg_msg(ERROR, "Failed to open file %s\n", file_path);
        free(file_path);
        return -1;
    }
    while (*stamp) {
        size = fread(stamp_buf, 1, STAMP_BUF_SIZE, file);
        if ((size == 0) && ferror(file)) {
            opkg_msg(ERROR, "Failed to read from file %s\n", file_path);
            diff = -1;
            break;
        }
        diff = ((size < STAMP_BUF_SIZE) && (size != (int)strlen(stamp)))
                || ((size == STAMP_BUF_SIZE) && (strlen(stamp) < STAMP_BUF_SIZE))
                || memcmp(stamp_buf, stamp, size);
        if (diff)
            break;
        stamp += STAMP_BUF_SIZE;
    }

    r = fclose(file);
    if (r != 0)
        opkg_msg(ERROR, "Failed to close file %s\n", file_path);
    free(file_path);
    return diff;
}

/** \brief opkg_validate_cached_file: check if file exists in cache
 *
 * \param src absolute URI of remote file
 * \param cache_location absolute name of cached file
 * \return 0 if file exists in cache and is completely downloaded.
 *         1 if file needs further downloading.
 *         -1 if error occurs.
 */
static int opkg_validate_cached_file(const char *src, const char *cache_location)
{
    CURLcode res;
    FILE *file;
    long resume_from = 0;
    char *etag = NULL;
    double src_size = -1;
    int ret = 1;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &dummy_write);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_write);
    curl_easy_setopt(curl, CURLOPT_WRITEHEADER, &etag);
    curl_easy_setopt(curl, CURLOPT_HEADER, 1);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1);  // remove body

    res = perform_curl_request(curl);
    if (res) {
        log_curl_download_error("Failed to download headers of",
                                src, res);
        ret = -1;
        goto cleanup;
    }
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &src_size);

    int match = 0;
    if (file_exists(cache_location)) {
        if (etag && (check_file_stamp(cache_location, etag) == 0))
            match = 1;
        else
            unlink(cache_location);
    }
    if (!match && etag) {
        int r = create_file_stamp(cache_location, etag);
        if (r != 0)
            opkg_msg(ERROR, "Failed to create stamp for %s.\n", cache_location);
    }

    file = fopen(cache_location, "ab");
    if (!file) {
        opkg_msg(ERROR, "Failed to open cache file %s\n", cache_location);
        ret = -1;
        goto cleanup;
    }
    fseek(file, 0, SEEK_END);
    resume_from = ftell(file);
    fclose(file);

    if (resume_from < src_size)
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM, resume_from);
    else {
        ret = 0;
        goto cleanup;
    }

cleanup:
    free(etag);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEHEADER, NULL);
    curl_easy_setopt(curl, CURLOPT_HEADER, 0);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0);
    return ret;
}

/* Download using curl backend. */
int opkg_download_backend(const char *src, const char *dest,
                          curl_progress_func cb, void *data, int use_cache)
{
    CURLcode res;
    FILE *file;
    int ret;

    curl = opkg_curl_init(cb, data);
    if (!curl)
        return -1;

    curl_easy_setopt(curl, CURLOPT_URL, src);

#ifdef HAVE_SSLCURL
    if (opkg_config->ftp_explicit_ssl) {
        /*
         * This is what enables explicit FTP SSL mode on curl's side As per
         * the official documentation at
         * http://curl.haxx.se/libcurl/c/curl_easy_setopt.html : "This
         * option was known as CURLOPT_FTP_SSL up to 7.16.4, and the
         * constants were known as CURLFTPSSL_*"
         */
        curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);

        /*
         * If a URL with the ftps:// scheme is passed to curl, then it
         * considers it's implicit mode. Thus, we need to fix it before
         * invoking curl.
         */
        char *fixed_src = replace_token_in_str(src, "ftps://", "ftp://");
        curl_easy_setopt(curl, CURLOPT_URL, fixed_src);
        free(fixed_src);
    }
#endif                          /* HAVE_SSLCURL */

    if (use_cache) {
        ret = opkg_validate_cached_file(src, dest);
        if (ret <= 0)
            return ret;
    } else {
        unlink(dest);
    }

    file = fopen(dest, "ab");
    if (!file) {
        opkg_msg(ERROR, "Failed to open destination file %s\n", dest);
        return -1;
    }

    res = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    res = perform_curl_request(curl);
    fclose(file);
    if (res) {
        log_curl_download_error("Failed to download", src, res);
        return -1;
    }

    return 0;
}

void opkg_download_cleanup(void)
{
    if (curl != NULL) {
        curl_easy_cleanup(curl);
        curl = NULL;
    }
    if (curl_errorbuffer != NULL) {
        free(curl_errorbuffer);
        curl_errorbuffer = NULL;
    }
}

/* This must be a macro as the third argument to curl_easy_setup has no
 * specified type.
 */
#define setopt(opt, value) do {                                     \
    int __r = curl_easy_setopt(curl, opt, value);                   \
    if (__r != CURLE_OK)                                            \
        opkg_msg(DEBUG, "Cannot set CURL option '%s'.\n", #opt);    \
} while (0)

static CURL *opkg_curl_init(curl_progress_func cb, void *data)
{
    if (curl == NULL) {
        curl = curl_easy_init();

        if (curl_errorbuffer == NULL) {
            curl_errorbuffer = xmalloc(CURL_ERROR_SIZE);
            setopt(CURLOPT_ERRORBUFFER, curl_errorbuffer);
        }

        // On high verbosity levels enable verbose libcurl output as well. It
        // will get printed to stderr.
        if (opkg_config->verbosity >= DEBUG) {
            setopt(CURLOPT_VERBOSE, 1);
        }

#ifdef HAVE_SSLCURL

        int r;

        if (opkg_config->ssl_engine) {
            /* use crypto engine */
            r = curl_easy_setopt(curl, CURLOPT_SSLENGINE,
                    opkg_config->ssl_engine);
            if (r != CURLE_OK) {
                opkg_msg(ERROR, "Can't set crypto engine '%s'.\n",
                         opkg_config->ssl_engine);

                opkg_download_cleanup();
                return NULL;
            }
            /* set the crypto engine as default */
            r = curl_easy_setopt(curl, CURLOPT_SSLENGINE_DEFAULT, 1L);
            if (r != CURLE_OK) {
                opkg_msg(ERROR, "Can't set crypto engine '%s' as default.\n",
                         opkg_config->ssl_engine);

                opkg_download_cleanup();
                return NULL;
            }
        }

        /* cert & key can only be in PEM case in the same file */
        if (opkg_config->ssl_key_passwd)
            setopt(CURLOPT_SSLKEYPASSWD, opkg_config->ssl_key_passwd);

        /* sets the client certificate and its type */
        if (opkg_config->ssl_cert_type)
            setopt(CURLOPT_SSLCERTTYPE, opkg_config->ssl_cert_type);

        /* SSL cert name isn't mandatory */
        if (opkg_config->ssl_cert)
            setopt(CURLOPT_SSLCERT, opkg_config->ssl_cert);

        /* sets the client key and its type */
        if (opkg_config->ssl_key_type)
            setopt(CURLOPT_SSLKEYTYPE, opkg_config->ssl_key_type);
        if (opkg_config->ssl_key)
            setopt(CURLOPT_SSLKEY, opkg_config->ssl_key);

        /* Should we verify the peer certificate ? */
        if (opkg_config->ssl_dont_verify_peer)
            /*
             * CURLOPT_SSL_VERIFYPEER default is nonzero (curl => 7.10)
             */
            setopt(CURLOPT_SSL_VERIFYPEER, 0);

        /* certification authority file and/or path */
        if (opkg_config->ssl_ca_file)
            setopt(CURLOPT_CAINFO, opkg_config->ssl_ca_file);
        if (opkg_config->ssl_ca_path)
            setopt(CURLOPT_CAPATH, opkg_config->ssl_ca_path);
#endif

        if (opkg_config->connect_timeout_ms > 0) {
            long timeout_ms = opkg_config->connect_timeout_ms;
            setopt(CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
        }

        if (opkg_config->transfer_timeout_ms > 0) {
            long timeout_ms = opkg_config->transfer_timeout_ms;
            setopt(CURLOPT_TIMEOUT_MS, timeout_ms);
        }

        if (opkg_config->follow_location)
            setopt(CURLOPT_FOLLOWLOCATION, 1);

        setopt(CURLOPT_FAILONERROR, 1);
        int use_proxy = opkg_config->http_proxy || opkg_config->ftp_proxy
                || opkg_config->https_proxy;
        if (use_proxy) {
            setopt(CURLOPT_PROXYUSERNAME, opkg_config->proxy_user);
            setopt(CURLOPT_PROXYPASSWORD, opkg_config->proxy_passwd);
            setopt(CURLOPT_PROXYAUTH, CURLAUTH_ANY);
        }
        if (opkg_config->http_auth) {
            setopt(CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            setopt(CURLOPT_USERPWD, opkg_config->http_auth);
        }
    }

    setopt(CURLOPT_NOPROGRESS, (cb == NULL));
    if (cb) {
        setopt(CURLOPT_PROGRESSDATA, data);
        setopt(CURLOPT_PROGRESSFUNCTION, cb);
    }

    return curl;
}

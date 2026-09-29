/* mime.c - extension to MIME type lookup */
#include "pweb.h"
#include <ctype.h>
#include <string.h>

static const struct { const char *ext; const char *mime; int bin; } MIME[] = {
    { "html", "text/html; charset=utf-8", 0 },
    { "htm",  "text/html; charset=utf-8", 0 },
    { "css",  "text/css", 0 },
    { "js",   "application/javascript", 0 },
    { "mjs",  "text/javascript", 0 },
    { "json", "application/json", 0 },
    { "txt",  "text/plain", 0 },
    { "log",  "text/plain", 0 },
    { "csv",  "text/csv", 0 },
    { "md",   "text/markdown", 0 },
    { "xml",  "application/xml", 0 },
    { "yaml", "application/yaml", 0 },
    { "yml",  "application/yaml", 0 },
    { "ini",  "text/plain", 0 },
    { "pdf",  "application/pdf", 1 },
    { "png",  "image/png", 1 },
    { "jpg",  "image/jpeg", 1 },
    { "jpeg", "image/jpeg", 1 },
    { "gif",  "image/gif", 1 },
    { "webp", "image/webp", 1 },
    { "avif", "image/avif", 1 },
    { "svg",  "image/svg+xml", 0 },
    { "ico",  "image/x-icon", 1 },
    { "bmp",  "image/bmp", 1 },
    { "tiff", "image/tiff", 1 },
    { "heic", "image/heic", 1 },
    { "mp3",  "audio/mpeg", 1 },
    { "wav",  "audio/wav", 1 },
    { "ogg",  "audio/ogg", 1 },
    { "flac", "audio/flac", 1 },
    { "m4a",  "audio/mp4", 1 },
    { "mp4",  "video/mp4", 1 },
    { "webm", "video/webm", 1 },
    { "mkv",  "video/x-matroska", 1 },
    { "avi",  "video/avi", 1 },
    { "mov",  "video/quicktime", 1 },
    { "zip",  "application/zip", 1 },
    { "gz",   "application/gzip", 1 },
    { "tar",  "application/x-tar", 1 },
    { "bz2",  "application/x-bzip2", 1 },
    { "xz",   "application/x-xz", 1 },
    { "7z",   "application/x-7z-compressed", 1 },
    { "rar",  "application/vnd.rar", 1 },
    { "doc",  "application/msword", 1 },
    { "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", 1 },
    { "xls",  "application/vnd.ms-excel", 1 },
    { "xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", 1 },
    { "ppt",  "application/vnd.ms-powerpoint", 1 },
    { "pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation", 1 },
    { "wasm", "application/wasm", 1 },
    { "bin",  "application/octet-stream", 1 },
    { "exe",  "application/x-msdownload", 1 },
    { "so",   "application/x-sharedlib", 1 },
    { "apk",  "application/vnd.android.package-archive", 1 },
    { "dmg",  "application/x-apple-diskimage", 1 },
    { "iso",  "application/x-iso9660-image", 1 },
    { "c",    "text/x-csrc", 0 },
    { "h",    "text/x-chdr", 0 },
    { "cpp",  "text/x-c++src", 0 },
    { "cc",   "text/x-c++src", 0 },
    { "py",   "text/x-python", 0 },
    { "sh",   "application/x-sh", 0 },
    { "lua",  "text/x-lua", 0 },
    { "go",   "text/x-go", 0 },
    { "rs",   "text/x-rust", 0 },
    { "java", "text/x-java", 0 },
    { "ts",   "text/typescript", 0 },
    { "tsx",  "text/tsx", 0 },
    { "jsx",  "text/jsx", 0 },
    { "sql",  "application/sql", 0 },
    { "php",  "application/octet-stream", 1 },
    { "class","application/octet-stream", 1 },
    { "wasm", "application/wasm", 1 },
    { NULL, NULL, 0 }
};

const char *mime_for(const char *path, int *is_binary)
{
    const char *dot = strrchr(path, '.');
    char ext[32];
    int elen = 0;
    if (dot && dot[1]) {
        const char *e = dot + 1;
        /* take up to 5 chars (tar.gz handled as .gz) */
        while (*e && elen < 4) { ext[elen++] = (char)tolower((unsigned char)*e); e++; }
        ext[elen] = 0;
    } else {
        ext[0] = 0;
    }
    for (int i = 0; MIME[i].ext; i++) {
        if (strcasecmp(ext, MIME[i].ext) == 0) {
            if (is_binary) { *is_binary = MIME[i].bin; }
            return MIME[i].mime;
        }
    }
    if (is_binary) { *is_binary = 0; }
    return "application/octet-stream";
}

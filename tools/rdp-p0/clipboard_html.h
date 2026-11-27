#pragma once
#include "clipboard_payload.h"
#include <stdio.h>

static const uint8_t *clip_find_html(const uint8_t *html, size_t length, const char *needle) {
    const size_t count = strlen(needle);
    if (count > length) return NULL;
    for (size_t i = 0; i <= length - count; i++) {
        size_t j = 0;
        for (; j < count; j++) {
            unsigned c = html[i+j];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (unsigned char)needle[j]) break;
        }
        if (j == count) return html + i;
    }
    return NULL;
}

static const uint8_t *clip_find_html_tag(const uint8_t *html, size_t length, const char *tag) {
    const size_t count = strlen(tag);
    const uint8_t *end = html + length;
    const uint8_t *found;
    while ((found = clip_find_html(html, length, tag)) != NULL) {
        html = found + count;
        if (html == end) break;
        if (*html && strchr("> \t\r\n", *html)) return found;
        length = (size_t)(end - html);
    }
    return NULL;
}

static int clip_is_html_table(const uint8_t *html, size_t length) {
    return html && length && length <= PROBE_CLIPBOARD_LIMIT &&
        clip_find_html_tag(html, length, "<table") &&
        clip_find_html(html, length, "</table>");
}

// Package the two representations in a single clipboard snapshot. Each RDP
// format request reads only its own representation from that snapshot.
static uint8_t *clip_pack_html(const uint8_t *html, size_t htmlLength,
                              const uint8_t *text, size_t textLength, size_t *size) {
    if (!htmlLength || htmlLength > PROBE_CLIPBOARD_LIMIT || textLength > PROBE_CLIPBOARD_LIMIT)
        return NULL;
    size_t terminatedLength = htmlLength + (html[htmlLength-1] != 0);
    if (terminatedLength > PROBE_CLIPBOARD_LIMIT) return NULL;
    uint8_t *data = (uint8_t *)malloc(4 + terminatedLength + textLength);
    if (!data) return NULL;
    clip_put32(data, (uint32_t)terminatedLength);
    memcpy(data + 4, html, htmlLength);
    data[4+terminatedLength-1] = 0;
    if (textLength) memcpy(data + 4 + terminatedLength, text, textLength);
    *size = 4 + terminatedLength + textLength;
    return data;
}

static size_t clip_html_size(const uint8_t *data, size_t size) {
    if (!data || size < 4) return 0;
    size_t count = clip_u32(data);
    return count <= size - 4 && count > 8 && count <= PROBE_CLIPBOARD_LIMIT &&
        !memcmp(data + 4, "Version:", 8) && data[4+count-1] == 0 &&
        clip_is_html_table(data + 4, count) ? count : 0;
}

// AppKit exports UTF-8 HTML without the CF_HTML header. Keep the enclosing
// HTML and styles. Excel uses the offsets to find the selected table.
static inline uint8_t *clip_wrap_html_table(const uint8_t *html, size_t length, size_t *size) {
    if (!clip_is_html_table(html, length) || memchr(html, 0, length)) return NULL;
    static const char startMarker[] = "<!--StartFragment-->";
    static const char endMarker[] = "<!--EndFragment-->";
    static const char header[] =
        "Version:1.0\r\nStartHTML:%010u\r\nEndHTML:%010u\r\nStartFragment:%010u\r\nEndFragment:%010u\r\n";
    const uint8_t *start = clip_find_html(html, length, "<!--startfragment-->");
    const uint8_t *end = clip_find_html(html, length, "<!--endfragment-->");
    const char *prefix = "", *suffix = "";
    size_t before, fragment, after;
    int insertMarkers = !start || !end || start + sizeof(startMarker)-1 >= end;
    if (insertMarkers) {
        start = clip_find_html_tag(html, length, "<table");
        // Use the last closing tag so nested tables remain within the fragment.
        for (end = start; ; ) {
            const uint8_t *next = clip_find_html(end, (size_t)(html + length - end), "</table>");
            if (!next) break;
            end = next + sizeof("</table>")-1;
        }
        if (end == start) return NULL;
    } else {
        start += sizeof(startMarker)-1;
    }
    if (!clip_find_html_tag(html, length, "<html")) {
        prefix = "<html><body>";
        suffix = "</body></html>";
    }
    before = (size_t)(start - html);
    fragment = (size_t)(end - start);
    after = length - before - fragment;
    size_t prefixLength = strlen(prefix), suffixLength = strlen(suffix);
    size_t startMarkerLength = insertMarkers ? sizeof(startMarker)-1 : 0;
    size_t endMarkerLength = insertMarkers ? sizeof(endMarker)-1 : 0;
    size_t headerLength = (size_t)snprintf(NULL, 0, header, 0u, 0u, 0u, 0u);
    size_t endHTML = headerLength + prefixLength + length + startMarkerLength + endMarkerLength + suffixLength;
    if (endHTML >= PROBE_CLIPBOARD_LIMIT) return NULL;
    size_t startFragment = headerLength + prefixLength + before + startMarkerLength;
    uint8_t *data = (uint8_t *)malloc(endHTML + 1);
    if (!data) return NULL;
    snprintf((char *)data, headerLength + 1, header, (unsigned)headerLength, (unsigned)endHTML,
             (unsigned)startFragment, (unsigned)(startFragment + fragment));
    uint8_t *body = data + headerLength;
    memcpy(body, prefix, prefixLength);
    memcpy(body + prefixLength, html, before);
    memcpy(body + prefixLength + before, startMarker, startMarkerLength);
    memcpy(data + startFragment, start, fragment);
    memcpy(data + startFragment + fragment, endMarker, endMarkerLength);
    memcpy(data + startFragment + fragment + endMarkerLength, end, after);
    memcpy(data + endHTML - suffixLength, suffix, suffixLength);
    data[endHTML] = 0;
    *size = endHTML + 1;
    return data;
}

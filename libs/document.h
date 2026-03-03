#ifndef DOCUMENT_H

#define DOCUMENT_H
/**
 * This file is the header file for all the document functions. You will be tested on the functions inside markdown.h
 * You are allowed to and encouraged multiple helper functions and data structures, and make your code as modular as possible. 
 * Ensure you DO NOT change the name of document struct.
 */

#include <stdint.h>
#include <stddef.h>

#define SUCCESS 0
#define INVALID_CURSOR_POS -1
#define DELETED_POSITION -2
#define OUTDATED_VERSION -3

#define INSERT 1
#define DELETE 2
#define OTHER 3
#define BLOCK_LEVEL 4
#define ORDERED_LIST 5
#define UNORDERED_LIST 6

#define MAX_FLATTENED_LENGTH 2048


typedef struct {
    size_t pos;
    size_t pos_end;
    int type;
    char *content;
    size_t content_length;
} Span;

typedef struct {
    char *text;
    size_t length;
    Span* spans;
    size_t span_count;

    char *flattened;
    size_t flattened_length;
    Span *staged_spans;
    size_t staged_span_count;

    uint64_t version;
} document;

// Functions from here onwards.
#endif
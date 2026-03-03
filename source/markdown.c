#include "../libs/markdown.h"
#include <stdlib.h>
#include <string.h>

// === Init and Free ===
document *markdown_init(){
    document *doc = malloc(sizeof(document));
    doc->text = malloc(1);
    doc->text[0] = '\0';
    doc->length = 0;
    doc->spans = NULL;
    doc->span_count = 0;

    doc->flattened = malloc(1);
    doc->flattened[0] = '\0';
    doc->flattened_length = 0;
    doc->staged_spans = NULL;
    doc->staged_span_count = 0;
    doc->version = 0;
    return doc;
}

void markdown_free(document *doc) {
    if (!doc) return;
    free(doc->text);
    for (size_t i = 0; i < doc->span_count; i++) {
        free(doc->spans[i].content);
    }
    free(doc->spans);
    free(doc->flattened);
    for (size_t i = 0; i < doc->staged_span_count; i++) {
        free(doc->staged_spans[i].content);
    }
    free(doc->staged_spans);
    free(doc);
}

// === Edit Commands ===
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *content) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            if (doc->staged_spans[i].pos <= pos && doc->staged_spans[i].pos_end >= pos) {
                pos = doc->staged_spans[i].pos;
            }
        }
    }

    Span *new_span = malloc(sizeof(Span));
    new_span->pos = pos;
    new_span->pos_end = 0;
    new_span->type = INSERT;
    new_span->content = strdup(content);
    new_span->content_length = strlen(content);
    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 1) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span;
    doc->staged_span_count++;
    free(new_span);

    return SUCCESS;
}

int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->flattened_length) return INVALID_CURSOR_POS;

    size_t pos_end = pos + len;
    if (pos_end > doc->flattened_length) pos_end = doc->flattened_length;

    Span *new_span = malloc(sizeof(Span));
    new_span->pos = pos;
    new_span->pos_end = pos_end;
    new_span->type = DELETE;
    new_span->content = NULL;
    new_span->content_length = 0;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 1) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span;
    doc->staged_span_count++;
    free(new_span);
    return SUCCESS;
}

// === Formatting Commands ===
int markdown_newline(document *doc, uint64_t version, size_t pos) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            if (doc->staged_spans[i].pos <= pos && doc->staged_spans[i].pos_end >= pos) {
                pos = doc->staged_spans[i].pos;
            }
        }
    }

    Span *new_span = malloc(sizeof(Span));
    new_span->pos = pos;
    new_span->pos_end = 0;
    new_span->type = OTHER;
    new_span->content = strdup("\n");
    new_span->content_length = 1;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 1) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span;
    doc->staged_span_count++;
    free(new_span);
    return SUCCESS;
}

int markdown_heading(document *doc, uint64_t version, size_t level, size_t pos) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->flattened_length - 1) return INVALID_CURSOR_POS;

    int start_newline = 0;
    if (pos != 0) {
        if (doc->flattened[pos-1] != '\n') start_newline = 1;
    }

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            if (doc->staged_spans[i].pos <= pos && doc->staged_spans[i].pos_end >= pos) {
                pos = doc->staged_spans[i].pos;
            }
        }
    }

    Span *new_span = malloc(sizeof(Span));

    if (level == 1) {
        if (start_newline) {
            new_span->content = strdup("\n# ");
            new_span->content_length = 4;
        }
        else {
            new_span->content = strdup("# ");
            new_span->content_length = 2;
        }
    }
    else if (level == 2) {
        if (start_newline) {
            new_span->content = strdup("\n## ");
            new_span->content_length = 4;
        }
        else {
            new_span->content = strdup("## ");
            new_span->content_length = 3;
        }
    }
    else if (level == 3) {
        if (start_newline) {
            new_span->content = strdup("\n### ");
            new_span->content_length = 5;
        }
        else {
            new_span->content = strdup("### ");
            new_span->content_length = 4;
        }
    }

    new_span->pos = pos;
    new_span->pos_end = 0;
    new_span->type = BLOCK_LEVEL;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 1) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span;
    doc->staged_span_count++;
    free(new_span);
    return SUCCESS;
}

int markdown_bold(document *doc, uint64_t version, size_t start, size_t end) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (start > doc->flattened_length) return INVALID_CURSOR_POS;
    if (end > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            int count = 0;
            if (doc->staged_spans[i].pos <= start && doc->staged_spans[i].pos_end >= start) {
                start = doc->staged_spans[i].pos_end;
                count++;
            }
            else if (doc->staged_spans[i].pos <= end && doc->staged_spans[i].pos_end >= end) {
                end = doc->staged_spans[i].pos;
                count++;
            }
            if (count == 2) return DELETED_POSITION;
        }
    }

    Span *new_span_start = malloc(sizeof(Span));
    Span *new_span_end = malloc(sizeof(Span));

    new_span_start->pos = start;
    new_span_start->pos_end = 0;
    new_span_start->type = OTHER;
    new_span_start->content = strdup("**");
    new_span_start->content_length = 2;

    new_span_end->pos = end;
    new_span_end->pos_end = 0;
    new_span_end->type = OTHER;
    new_span_end->content = strdup("**");
    new_span_end->content_length = 2;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 2) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span_start;
    doc->staged_spans[doc->staged_span_count + 1] = *new_span_end;
    doc->staged_span_count += 2;
    free(new_span_start);
    free(new_span_end);
    return SUCCESS;
}

int markdown_italic(document *doc, uint64_t version, size_t start, size_t end) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (start > doc->flattened_length) return INVALID_CURSOR_POS;
    if (end > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            int count = 0;
            if (doc->staged_spans[i].pos <= start && doc->staged_spans[i].pos_end >= start) {
                start = doc->staged_spans[i].pos_end;
                count++;
            }
            else if (doc->staged_spans[i].pos <= end && doc->staged_spans[i].pos_end >= end) {
                end = doc->staged_spans[i].pos;
                count++;
            }
            if (count == 2) return DELETED_POSITION;
        }
    }

    Span *new_span_start = malloc(sizeof(Span));
    Span *new_span_end = malloc(sizeof(Span));

    new_span_start->pos = start;
    new_span_start->pos_end = 0;
    new_span_start->type = OTHER;
    new_span_start->content = strdup("*");
    new_span_start->content_length = 2;

    new_span_end->pos = end;
    new_span_end->pos_end = 0;
    new_span_end->type = OTHER;
    new_span_end->content = strdup("*");
    new_span_end->content_length = 2;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 2) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span_start;
    doc->staged_spans[doc->staged_span_count + 1] = *new_span_end;
    doc->staged_span_count += 2;
    free(new_span_start);
    free(new_span_end);
    return SUCCESS;
}

int markdown_blockquote(document *doc, uint64_t version, size_t pos) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            if (doc->staged_spans[i].pos <= pos && doc->staged_spans[i].pos_end >= pos) {
                pos = doc->staged_spans[i].pos;
            }
        }
    }

    int start_newline = 0;
    if (pos != 0) {
        if (doc->flattened[pos-1] != '\n') start_newline = 1;
    }

    Span *new_span = malloc(sizeof(Span));

    if (start_newline) {
        new_span->content = strdup("\n> ");
        new_span->content_length = 3;
    }
    else {
        new_span->content = strdup("> ");
        new_span->content_length = 2;
    }
    new_span->pos = pos;
    new_span->pos_end = 0;
    new_span->type = BLOCK_LEVEL;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 1) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span;
    doc->staged_span_count++;
    free(new_span);
    return SUCCESS;
}

int markdown_ordered_list(document *doc, uint64_t version, size_t pos) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            if (doc->staged_spans[i].pos <= pos && doc->staged_spans[i].pos_end >= pos) {
                pos = doc->staged_spans[i].pos;
            }
        }
    }

    int start_newline = 0;
    if (pos != 0) {
        if (doc->flattened[pos-1] != '\n') start_newline = 1;
    }

    Span *new_span = malloc(sizeof(Span));
    
    if (start_newline) {
        new_span->content = strdup("\n1. ");
        new_span->content_length = 4;
    }
    else {
        new_span->content = strdup("1. ");
        new_span->content_length = 3;
    }

    new_span->pos = pos;
    new_span->pos_end = 0;
    new_span->type = ORDERED_LIST;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 1) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span;
    doc->staged_span_count++;
    free(new_span); 
    return SUCCESS;
}

int markdown_unordered_list(document *doc, uint64_t version, size_t pos) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            if (doc->staged_spans[i].pos <= pos && doc->staged_spans[i].pos_end >= pos) {
                pos = doc->staged_spans[i].pos;
            }
        }
    }

    int start_newline = 0;
    if (pos != 0) {
        if (doc->flattened[pos-1] != '\n') start_newline = 1;
    }

    Span *new_span = malloc(sizeof(Span));
    if (start_newline) {
        new_span->content = strdup("\n- ");
        new_span->content_length = 4;
    }
    else {
        new_span->content = strdup("- ");
        new_span->content_length = 2;
    }

    new_span->pos = pos;
    new_span->pos_end = 0;
    new_span->type = UNORDERED_LIST;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 1) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span;
    doc->staged_span_count++;
    free(new_span);
    return SUCCESS;
}

int markdown_code(document *doc, uint64_t version, size_t start, size_t end) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (start > doc->flattened_length) return INVALID_CURSOR_POS;
    if (end > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            int count = 0;
            if (doc->staged_spans[i].pos <= start && doc->staged_spans[i].pos_end >= start) {
                start = doc->staged_spans[i].pos_end;
                count++;
            }
            else if (doc->staged_spans[i].pos <= end && doc->staged_spans[i].pos_end >= end) {
                end = doc->staged_spans[i].pos;
                count++;
            }
            if (count == 2) return DELETED_POSITION;
        }
    }

    Span *new_span_start = malloc(sizeof(Span));
    Span *new_span_end = malloc(sizeof(Span));

    new_span_start->pos = start;
    new_span_start->pos_end = 0;
    new_span_start->type = OTHER;
    new_span_start->content = strdup("`");
    new_span_start->content_length = 1;

    new_span_end->pos = end;
    new_span_end->pos_end = 0;
    new_span_end->type = OTHER;
    new_span_end->content = strdup("`");
    new_span_end->content_length = 1;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 2) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span_start;
    doc->staged_spans[doc->staged_span_count + 1] = *new_span_end;
    doc->staged_span_count += 2;
    free(new_span_start);
    free(new_span_end);
    return SUCCESS;
}

int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            if (doc->staged_spans[i].pos <= pos && doc->staged_spans[i].pos_end >= pos) {
                pos = doc->staged_spans[i].pos;
            }
        }
    }

    int start_newline = 0;
    int after_newline = 0;


    if (pos != 0 && doc->flattened[pos-1] != '\n') {
        start_newline = 1;
    }
    if (pos == doc->flattened_length || doc->flattened[pos] != '\n') {
        after_newline = 1;
    }

    Span *new_span = malloc(sizeof(Span));
    if (start_newline && after_newline) {
        new_span->content = strdup("\n---\n");
        new_span->content_length = 5;
    }
    else if (start_newline) {
        new_span->content = strdup("\n---");
        new_span->content_length = 4;
    }
    else if (after_newline) {
        new_span->content = strdup("---\n");
        new_span->content_length = 4;
    }
    else {
        new_span->content = strdup("---");
        new_span->content_length = 3;
    }

    new_span->pos = pos;
    new_span->pos_end = 0;
    new_span->type = OTHER;

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 1) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span;
    doc->staged_span_count++;
    free(new_span);
    return SUCCESS;
}

int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url) {
    if (version != doc->version) return OUTDATED_VERSION;
    if (start > doc->flattened_length) return INVALID_CURSOR_POS;
    if (end > doc->flattened_length) return INVALID_CURSOR_POS;

    for (size_t i = 0; i < doc->staged_span_count; i++) {
        if (doc->staged_spans[i].type == DELETE) {
            int count = 0;
            if (doc->staged_spans[i].pos <= start && doc->staged_spans[i].pos_end >= start) {
                start = doc->staged_spans[i].pos_end;
                count++;
            }
            else if (doc->staged_spans[i].pos <= end && doc->staged_spans[i].pos_end >= end) {
                end = doc->staged_spans[i].pos;
                count++;
            }
            if (count == 2) return DELETED_POSITION;
        }
    }

    Span *new_span_start = malloc(sizeof(Span));
    Span *new_span_end = malloc(sizeof(Span));

    new_span_start->pos = start;
    new_span_start->pos_end = 0;
    new_span_start->type = OTHER;
    new_span_start->content = strdup("[");
    new_span_start->content_length = 1;

    new_span_end->pos = end;
    new_span_end->pos_end = 0;
    new_span_end->type = OTHER;
    char *content = malloc(strlen(url) + 4);
    snprintf(content, strlen(url) + 4, "](%s)", url);
    new_span_end->content = content;
    new_span_end->content_length = strlen(content);

    doc->staged_spans = realloc(doc->staged_spans, (doc->staged_span_count + 2) * sizeof(Span));
    doc->staged_spans[doc->staged_span_count] = *new_span_start;
    doc->staged_spans[doc->staged_span_count + 1] = *new_span_end;
    doc->staged_span_count += 2;
    free(new_span_start);
    free(new_span_end);
    return SUCCESS;
}

// === Utilities ===
void markdown_print(const document *doc, FILE *stream) {   //flatten 함수 부르고, 결과를 파일 스트림에 넘겨주기
    fwrite(doc->flattened, 1, doc->flattened_length, stream);
    fwrite("\n", 1, 1, stream);
    return;
}

char *markdown_flatten(const document *doc) {
    char *flattened = malloc(MAX_FLATTENED_LENGTH);
    flattened[0] = '\0';

    size_t *delete_pos = malloc(doc->length * sizeof(size_t));
    memset(delete_pos, 0, doc->length * sizeof(size_t));
    
    for (size_t i = 0; i < doc->span_count; i++) {
        if (doc->spans[i].type == DELETE) {
            for (size_t j = doc->spans[i].pos; j < doc->spans[i].pos_end; j++) {
                delete_pos[j] = 1;
            }
        }
    }

    for (size_t i = 0; i <= doc->length; i++) {
        for (int j = (int)doc->span_count - 1; j >= 0; j--) {
            if (doc->spans[j].type == INSERT && doc->spans[j].pos == i) {
                if (strlen(flattened) + doc->spans[j].content_length >= MAX_FLATTENED_LENGTH - 1) {
                    char* tmp = realloc(flattened, strlen(flattened) + MAX_FLATTENED_LENGTH);
                    if (tmp == NULL) {
                        free(flattened);
                        free(delete_pos);
                        return NULL;
                    }
                    flattened = tmp;
                }
                strcat(flattened, doc->spans[j].content);
            }
        }
        for (size_t j = 0; j < doc->span_count; j++) {
            if ((doc->spans[j].type == OTHER || doc->spans[j].type == BLOCK_LEVEL || doc->spans[j].type == ORDERED_LIST 
            || doc->spans[j].type == UNORDERED_LIST) && doc->spans[j].pos == i) {
                if (strlen(flattened) + strlen(doc->spans[j].content) >= MAX_FLATTENED_LENGTH - 1) {
                    char* tmp = realloc(flattened, strlen(flattened) + MAX_FLATTENED_LENGTH);
                    if (tmp == NULL) {
                        free(flattened);
                        free(delete_pos);
                        return NULL;
                    }
                    flattened = tmp;
                }
                strcat(flattened, doc->spans[j].content);
            }
        }
        if (i < doc->length && delete_pos[i] != 1) {
            if (strlen(flattened) + 1 >= MAX_FLATTENED_LENGTH - 1) {
                char* tmp = realloc(flattened, strlen(flattened) + MAX_FLATTENED_LENGTH);
                if (tmp == NULL) {
                    free(flattened);
                    free(delete_pos);
                    return NULL;
                }
                flattened = tmp;
            }
            strncat(flattened, &doc->text[i], 1);
        }
    }

    size_t flattened_len = strlen(flattened);
    int current_number = 1;
    
    for (size_t i = 0; i < flattened_len; i++) {
        if (i < flattened_len - 1 && flattened[i] >= '1' && flattened[i] <= '9' && flattened[i + 1] == '.') {
            flattened[i] = '0' + current_number;
            current_number++;
            
            while (i < flattened_len && flattened[i] != '\n') {
                i++;
            }
        }
        else if (flattened[i] == '\n') {
            if (i + 1 < flattened_len) {
                if (!(flattened[i + 1] >= '1' && flattened[i + 1] <= '9' && 
                      i + 2 < flattened_len && flattened[i + 2] == '.')) {
                    current_number = 1;
                }
            }
        }
    }

    free(delete_pos);
    return flattened;
}

// === Versioning ===
void markdown_increment_version(document *doc) {
    doc->version++;
    free(doc->text);
    for (size_t i = 0; i < doc->span_count; i++) {
        free(doc->spans[i].content);
    }
    free(doc->spans);
    doc->text = doc->flattened;
    doc->length = strlen(doc->text);
    doc->spans = doc->staged_spans;
    doc->span_count = doc->staged_span_count;

    doc->flattened = markdown_flatten(doc);
    doc->flattened_length = strlen(doc->flattened);
    doc->staged_spans = NULL;
    doc->staged_span_count = 0;
    return;
}

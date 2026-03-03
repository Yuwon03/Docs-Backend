//TODO: client code that can send instructions to server.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>  
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <ctype.h>
#include "../libs/markdown.h"

static int fifo_fd_c2s;
static int fifo_fd_s2c;

static document *client_doc = NULL;
static char *client_log = NULL;
static size_t log_len = 0;
static size_t log_cap = 0;
static char role[32] = {0};

void handle_client_log(const char *msg){
    size_t m = strlen(msg);
    if (log_len + m + 1 > log_cap){
        size_t new_cap = log_cap ? log_cap * 2 : 1024;
        while (new_cap < log_len + m + 1) new_cap *= 2;
        char *tmp = realloc(client_log, new_cap);
        if (!tmp) {
            return;
        }
        client_log = tmp;
        log_cap = new_cap;
    }
    memcpy(client_log + log_len, msg, m);
    log_len += m;
    client_log[log_len] = '\0';
    return;
}

void display_client_log(){
    if (client_log){
        fputs(client_log, stdout);
        fflush(stdout);
    }
}

ssize_t read_newline(int fd, char *buf, size_t max){
    size_t i = 0;
    while (i + 1 < max) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) return -1;
        if (r == 0) return 0;
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

void *reader_thread(void *arg){
    (void)arg;
    int updated = 0;
    char line[256];
    ssize_t n = 0;
    while ((n = read_newline(fifo_fd_s2c, line, sizeof(line))) > 0){
        handle_client_log(line);
        if (strcmp(line, "END\n") == 0){
            if (updated) {
                updated = 0;
                markdown_increment_version(client_doc);
            }
            continue;
        }
        if (strncmp(line, "VERSION", 7) == 0){
            if (client_doc->version != strtoull(line+8, NULL, 10)){
                updated = 1;
            }
            continue;
        } 
        if (strncmp(line, "EDIT ", 5) == 0){
            fputs(line, stdout);
            fflush(stdout);
            line[strcspn(line, "\n")] = '\0';
            char *last = strrchr(line, ' ');
            if (!last) continue;
            char *status = last + 1;
            if (strcmp(status, "SUCCESS") != 0){ continue; }
            *last = '\0';

            char *saveptr;
            char *p = line + 5;
            strtok_r(p, " ", &saveptr);
            char *cmd = strtok_r(NULL, " ", &saveptr);
            char *pos_s = strtok_r(NULL, " ", &saveptr);
            char *content = saveptr;

            size_t pos = strtoul(pos_s, NULL, 10);
            if (strcmp(cmd, "INSERT") == 0){
                markdown_insert(client_doc, client_doc->version, pos, content);
            }
            else if (strcmp(cmd, "DELETE") == 0){
                size_t length = strtoul(content, NULL, 10);
                markdown_delete(client_doc, client_doc->version, pos, length);
            }
            else if (strcmp(cmd, "NEWLINE") == 0){
                markdown_newline(client_doc, client_doc->version, pos);
            }
            else if (strcmp(cmd, "HEADING") == 0){
                size_t level = strtoul(content, NULL, 10);
                markdown_heading(client_doc, client_doc->version, level, pos);
            } 
            else if (strcmp(cmd, "BOLD") == 0){
                size_t end = strtol(content, NULL, 10);
                markdown_bold(client_doc, client_doc->version, pos, end);
            }
            else if (strcmp(cmd, "ITALIC") == 0){
                size_t end = strtol(content, NULL, 10);
                markdown_italic(client_doc, client_doc->version, pos, end);
            }
            else if (strcmp(cmd, "BLOCKQUOTE") == 0){   
                markdown_blockquote(client_doc, client_doc->version, pos);
            }
            else if (strcmp(cmd, "ORDERED_LIST") == 0){
                markdown_ordered_list(client_doc, client_doc->version, pos);
            }
            else if (strcmp(cmd, "UNORDERED_LIST") == 0){
                markdown_unordered_list(client_doc, client_doc->version, pos);
            }
            else if (strcmp(cmd, "CODE") == 0){
                size_t end = strtol(content, NULL, 10);
                markdown_code(client_doc, client_doc->version, pos, end);
            }
            else if (strcmp(cmd, "HORIZONTAL_RULE") == 0){
                markdown_horizontal_rule(client_doc, client_doc->version, pos);
            } 
            else if (strcmp(cmd, "LINK") == 0){
                char *end_c = strtok_r(NULL, " ", &saveptr);
                size_t end = strtoul(end_c, NULL, 10);
                char *url = saveptr; 
                markdown_link(client_doc, client_doc->version, pos, end, url);
            }
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <Server PId> <username>\n", argv[0]);
        return 1;
    }
    //Get PID & username
    pid_t client_pid = getpid();
    union sigval sv;
    pid_t server_pid = atoi(argv[1]);
    char username[20];
    strncpy(username, argv[2], sizeof(username)-1);
    username[sizeof(username)-1] = '\0';

    struct timespec timeout = { .tv_sec = 1, .tv_nsec = 0 };
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN+1);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sv.sival_int = client_pid;
    sigqueue(server_pid, SIGRTMIN, sv);
    if (sigtimedwait(&mask, NULL, &timeout) == -1) {
        printf("Timeout waiting for server response\n");
        return 1;
    }
    // open FIFOs
    char fifo_name_c2s[64];
    char fifo_name_s2c[64];
    sprintf(fifo_name_c2s, "./FIFO_C2S_%d", client_pid);
    sprintf(fifo_name_s2c, "./FIFO_S2C_%d", client_pid);

    fifo_fd_c2s = open(fifo_name_c2s, O_WRONLY);
    fifo_fd_s2c = open(fifo_name_s2c, O_RDONLY);

    if (fifo_fd_c2s < 0 || fifo_fd_s2c < 0) {
        if (fifo_fd_c2s >= 0) close(fifo_fd_c2s);
        if (fifo_fd_s2c >= 0) close(fifo_fd_s2c);

        snprintf(fifo_name_c2s, sizeof(fifo_name_c2s), "/tmp/FIFO_C2S_%d", client_pid);
        snprintf(fifo_name_s2c, sizeof(fifo_name_s2c), "/tmp/FIFO_S2C_%d", client_pid);

        fifo_fd_c2s = open(fifo_name_c2s, O_WRONLY);
        fifo_fd_s2c = open(fifo_name_s2c, O_RDONLY);
    }
    if (fifo_fd_c2s == -1 || fifo_fd_s2c == -1) {
        perror("open");
        return 1;
    }

    //Send username to server
    ssize_t w = write(fifo_fd_c2s, username, strlen(username));
    if (w < 0) {
        perror("write");
        return 1;
    }

    //Wait for server response
    size_t bytes_read = read(fifo_fd_s2c, role, sizeof(role) - 1);
    if (bytes_read <= 0) {
        return 1;
    }
    role[bytes_read] = '\0';
    if (strcmp(role, "Reject UNAUTHORISED\n") == 0) {
        fprintf(stdout, "%s", role);
        return 1;
    }

    //Read version
    char line[256];
    if (read_newline(fifo_fd_s2c, line, sizeof(line)) <= 0) {
        return 1;
    }
    uint64_t version = strtoull(line, NULL, 10);
    //Read document length
    if (read_newline(fifo_fd_s2c, line, sizeof(line)) <= 0) {
        return 1;
    }
    size_t length = strtoull(line, NULL, 10);
    //Read document
    char *document = malloc(length + 1);
    if (!document) {
        return 1;
    }
    ssize_t r = read(fifo_fd_s2c, document, length);
    if (r < 0) {
        perror("read");
        return 1;
    }
    document[length] = '\0';

    //Initialize document
    client_doc = markdown_init();
    if (!client_doc) {
        free(document);
        exit(1);
    }
    free(client_doc->text); 
    free(client_doc->flattened);  
    client_doc->text = malloc(1);
    client_doc->text[0] = '\0';
    client_doc->length = 0;
    client_doc->flattened = document;
    client_doc->flattened_length = length;
    client_doc->spans = NULL;
    client_doc->span_count = 0;
    client_doc->version = version;

    pthread_t tid;
    pthread_create(&tid, NULL, reader_thread, NULL);

    char command[128];
    while (fgets(command, sizeof(command), stdin) != NULL){
        ssize_t r = strlen(command);
        if (strcmp(command, "DOC?\n") == 0){
            fputs(client_doc->flattened, stdout);
            fputs("\n", stdout);
            fflush(stdout);
        }
        else if (strcmp(command, "PERM?\n") == 0){
            fputs(role, stdout);
            fflush(stdout);
        }
        else if (strcmp(command, "LOG?\n") == 0){
            display_client_log();
        }
        else {
            command[strcspn(command, "\n")] = '\0';
            if (strcmp(command, "DISCONNECT") == 0){
                ssize_t w = write(fifo_fd_c2s, command, r);
                if (w < 0) {
                    perror("write");
                    break;
                }
                break;
            } else {
                ssize_t w = write(fifo_fd_c2s, command, r);
                if (w < 0) {
                    perror("write");
                }
            }
        }
    }

    markdown_free(client_doc);
    free(client_log);
    close(fifo_fd_c2s);
    close(fifo_fd_s2c);

    return 0;
}
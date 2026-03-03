// TODO: server code that manages the document and handles client instructions
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>  
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>     
#include <unistd.h>    
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include <time.h>
#include "../libs/markdown.h"

#define rolefile "roles.txt"
#define MAX_COMMAND_LENGTH 256


typedef struct Client {
    int fifo_fd_s2c;
    int fifo_fd_c2s;
    char fifo_name_c2s[64];
    char fifo_name_s2c[64];
    char username[64];
    int role;
    int quit;
    struct Client *next;
} Client;

typedef struct Command {
    char *line;
    char username[64];
    int role;
    struct timespec ts;
    struct Command *next;
} Command;

static volatile sig_atomic_t stop_requested = 0;
int pthread_amount = 0;

static Client *clients= NULL;
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

static Command *cmd_head = NULL;
static Command *cmd_tail = NULL;
static pthread_mutex_t cmd_mtx = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t doc_mtx = PTHREAD_MUTEX_INITIALIZER;

document *server_doc = NULL;

static char *server_log = NULL;
static size_t server_log_len = 0;
static size_t server_log_cap = 0;
static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;

void add_to_server_log(const char *msg) {
    pthread_mutex_lock(&log_mtx);
    size_t msg_len = strlen(msg);
    if (server_log_len + msg_len + 1 > server_log_cap) {
        size_t new_cap = server_log_cap ? server_log_cap * 2 : 1024;
        while (new_cap < server_log_len + msg_len + 1) new_cap *= 2;
        char *tmp = realloc(server_log, new_cap);
        if (!tmp) {
            pthread_mutex_unlock(&log_mtx);
            return;
        }
        server_log = tmp;
        server_log_cap = new_cap;
    }
    memcpy(server_log + server_log_len, msg, msg_len);
    server_log_len += msg_len;
    server_log[server_log_len] = '\0';
    pthread_mutex_unlock(&log_mtx);
}

void display_server_log() {
    pthread_mutex_lock(&log_mtx);
    if (server_log) {
        fputs(server_log, stdout);
        fflush(stdout);
    }
    pthread_mutex_unlock(&log_mtx);
}

void cleanup_handler(int sig) {
    (void)sig;
    stop_requested = 1;
}

void register_client(Client *client) {
    pthread_mutex_lock(&clients_mtx);
    client->next = clients;
    clients = client;
    pthread_mutex_unlock(&clients_mtx);
}

void unregister_client(Client *client) {
    pthread_mutex_lock(&clients_mtx);
    Client **p = &clients;
    while (*p) {
        if (*p == client) {
            *p = client->next;
            close(client->fifo_fd_s2c);
            close(client->fifo_fd_c2s);
            unlink(client->fifo_name_c2s);
            unlink(client->fifo_name_s2c);
            free(client);
            break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&clients_mtx);
}

void enqueue_command(const char *line, Client *client) {
    Command *cmd = malloc(sizeof(Command));
    if (!cmd) return;
    clock_gettime(CLOCK_MONOTONIC, &cmd->ts);
    cmd->line = strdup(line);
    strncpy(cmd->username, client->username, sizeof(cmd->username) - 1);
    cmd->username[sizeof(cmd->username) - 1] = '\0';
    cmd->role = client->role;
    cmd->next = NULL;

    pthread_mutex_lock(&cmd_mtx);
    if (!cmd_head) cmd_head = cmd;
    else cmd_tail->next = cmd;
    cmd_tail = cmd;
    pthread_mutex_unlock(&cmd_mtx);
}

Command *drain_all_commands(void) {
    pthread_mutex_lock(&cmd_mtx);
    Command *batch = cmd_head;
    cmd_head = NULL;
    cmd_tail = NULL;
    pthread_mutex_unlock(&cmd_mtx);
    return batch;
}

int cmp_cmd_ts(const void *a, const void *b) {
    const Command *x = *(const Command**)a;
    const Command *y = *(const Command**)b;
    if (x->ts.tv_sec  < y->ts.tv_sec ) return -1;
    if (x->ts.tv_sec  > y->ts.tv_sec ) return +1;
    if (x->ts.tv_nsec < y->ts.tv_nsec) return -1;
    if (x->ts.tv_nsec > y->ts.tv_nsec) return +1;
    return 0;
}

void *stdin_thread(void *arg) {
    (void)arg;
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, "DOC?") == 0) {
            pthread_mutex_lock(&doc_mtx);
            markdown_print(server_doc, stdout);
            fflush(stdout);
            pthread_mutex_unlock(&doc_mtx);
        }
        else if (strcmp(line, "LOG?") == 0) {
            display_server_log();
        }
        else if (strcmp(line, "QUIT") == 0) {
            if (pthread_amount > 0) {
                fprintf(stdout, "QUIT rejected, %d clients still connected\n", pthread_amount);
                fflush(stdout);
            }
            else {
                kill(getpid(), SIGINT);
            }
        }
    }
    return NULL;
}

void *edit_document(void *arg) {
    long interval_ms = (long)arg;
    static char **pending_responses = NULL;
    static size_t *pending_lens = NULL;
    static size_t pending_count = 0;
    static int updated = 0;

    while (!stop_requested) {
        usleep(interval_ms * 1000);

        char header[32];
        pthread_mutex_lock(&doc_mtx);
        uint64_t curr_version = server_doc->version;
        pthread_mutex_unlock(&doc_mtx);
        int hlen = snprintf(header, sizeof(header), "VERSION %" PRIu64 "\n", curr_version);

        size_t br_count = pending_count + 2;
        char **broadcast = malloc(br_count * sizeof(*broadcast));
        size_t *broadcast_lens = malloc(br_count * sizeof(*broadcast_lens));
        // header
        broadcast[0] = malloc(hlen + 1);
        memcpy(broadcast[0], header, hlen + 1);
        broadcast_lens[0] = hlen;
        for (size_t i = 0; i < pending_count; i++) {
            broadcast[i + 1] = pending_responses[i];
            broadcast_lens[i + 1] = pending_lens[i];
        }
        // END marker
        broadcast[br_count - 1] = strdup("END\n");
        broadcast_lens[br_count - 1] = strlen("END\n");

        // add to server log
        for (size_t i = 0; i < br_count; i++) {
            add_to_server_log(broadcast[i]);
        }

        // send to log to clients
        pthread_mutex_lock(&clients_mtx);
        for (Client *c = clients; c; c = c->next) {
            for (size_t i = 0; i < br_count; i++) {
                ssize_t w = write(c->fifo_fd_s2c, broadcast[i], broadcast_lens[i]);
                if (w < 0) {
                    break;
                }
            }
        }
        pthread_mutex_unlock(&clients_mtx);


        for (size_t i = 0; i < br_count; i++) {
            free(broadcast[i]);
        }
        free(broadcast);
        free(broadcast_lens);
        free(pending_responses);
        free(pending_lens);

        Command *batch = drain_all_commands();
        size_t n = 0;
        for (Command *c = batch; c; c = c->next) n++;
        Command **arr = malloc(n * sizeof(*arr));
        size_t idx = 0;
        for (Command *c = batch; c; c = c->next) arr[idx++] = c;
        qsort(arr, n, sizeof(*arr), cmp_cmd_ts);

        pending_responses = malloc((n + 1) * sizeof(*pending_responses));
        pending_lens = malloc((n + 1) * sizeof(*pending_lens));
        pending_count = 0;

        for (size_t i = 0; i < n; i++) {
            Command *c = arr[i];
            char *line = c->line;
            char *username = c->username;
            int role = c->role;

            char bufcpy[MAX_COMMAND_LENGTH];
            strncpy(bufcpy, line, sizeof(bufcpy));
            bufcpy[strcspn(bufcpy, "\n")] = '\0';

            char *saveptr;
            char *command = strtok_r(bufcpy, " ", &saveptr);
            int result = 0;
            if(strcmp(command, "DISCONNECT") == 0) {
                Client *me = NULL;
                pthread_mutex_lock(&clients_mtx);
                for (Client *c = clients; c; c = c->next) {
                    if (strcmp(c->username, username) == 0) {
                        me = c;
                        break;
                    }
                }
                pthread_mutex_unlock(&clients_mtx);
                if (me) {
                    me->quit = 1;
                    unregister_client(me);
                    pthread_amount--;
                }
                continue;
            }
            if (role == 1) {
                pthread_mutex_lock(&doc_mtx);
                if (strcmp(command, "INSERT") == 0) {
                    char *pos = strtok_r(NULL, " ", &saveptr);
                    char *content = strtok_r(NULL, "", &saveptr);
                    size_t pos_int = strtoul(pos, NULL, 10);
                    result = markdown_insert(server_doc, server_doc->version, pos_int, content);
                }
                else if(strcmp(command, "DELETE") == 0) {
                    char *pos = strtok_r(NULL, " ", &saveptr);
                    char *len = strtok_r(NULL, "", &saveptr);
                    size_t pos_int = strtoul(pos, NULL, 10);
                    size_t len_int = strtoul(len, NULL, 10);
                    result = markdown_delete(server_doc, server_doc->version, pos_int, len_int);
                }
                else if(strcmp(command, "NEWLINE") == 0) {
                    char *pos = strtok_r(NULL, " ", &saveptr);
                    size_t pos_int = strtoul(pos, NULL, 10);
                    result = markdown_newline(server_doc, server_doc->version, pos_int);
                }
                else if(strcmp(command, "HEADING") == 0) {
                    char *level = strtok_r(NULL, " ", &saveptr);
                    char *pos = strtok_r(NULL, "", &saveptr);
                    int level_int = strtoul(level, NULL, 10);
                    size_t pos_int = strtoul(pos, NULL, 10);
                    result = markdown_heading(server_doc, server_doc->version, level_int, pos_int);
                }
                else if(strcmp(command, "BOLD") == 0) {
                    char *pos_start = strtok_r(NULL, " ", &saveptr);
                    char *pos_end = strtok_r(NULL, "", &saveptr);
                    size_t pos_start_i = strtoul(pos_start, NULL, 10);
                    size_t pos_end_i = strtoul(pos_end, NULL, 10);
                    result = markdown_bold(server_doc, server_doc->version, pos_start_i, pos_end_i);
                }
                else if(strcmp(command, "ITALIC") == 0) {
                    char *pos_start = strtok_r(NULL, " ", &saveptr);
                    char *pos_end = strtok_r(NULL, "", &saveptr);
                    size_t pos_start_i = strtoul(pos_start, NULL, 10);
                    size_t pos_end_i = strtoul(pos_end, NULL, 10);
                    result = markdown_italic(server_doc, server_doc->version, pos_start_i, pos_end_i);
                }
                else if(strcmp(command, "BLOCKQUOTE") == 0) {
                    char *pos = strtok_r(NULL, " ", &saveptr);
                    size_t pos_int = strtoul(pos, NULL, 10);
                    result = markdown_blockquote(server_doc, server_doc->version, pos_int);
                }
                else if(strcmp(command, "ORDERED_LIST") == 0) {
                    char *pos = strtok_r(NULL, " ", &saveptr);
                    size_t pos_int = strtoul(pos, NULL, 10);
                    result = markdown_ordered_list(server_doc, server_doc->version, pos_int);
                }
                else if(strcmp(command, "UNORDERED_LIST") == 0) {
                    char *pos = strtok_r(NULL, " ", &saveptr);
                    size_t pos_int = strtoul(pos, NULL, 10);
                    result = markdown_unordered_list(server_doc, server_doc->version, pos_int);
                }
                else if(strcmp(command, "CODE") == 0) {
                    char *pos_start = strtok_r(NULL, " ", &saveptr);
                    char *pos_end = strtok_r(NULL, "", &saveptr);
                    size_t pos_start_i = strtoul(pos_start, NULL, 10);
                    size_t pos_end_i = strtoul(pos_end, NULL, 10);
                    result = markdown_code(server_doc, server_doc->version, pos_start_i, pos_end_i);
                }
                else if(strcmp(command, "HORIZONTAL_RULE") == 0) {
                    char *pos = strtok_r(NULL, " ", &saveptr);
                    size_t pos_int = strtoul(pos, NULL, 10);
                    result = markdown_horizontal_rule(server_doc, server_doc->version, pos_int);
                }
                else if(strcmp(command, "LINK") == 0) {
                    char *pos_start = strtok_r(NULL, " ", &saveptr);
                    char *pos_end = strtok_r(NULL, " ", &saveptr);
                    char *link = strtok_r(NULL, "", &saveptr);
                    size_t pos_start_i = strtoul(pos_start, NULL, 10);
                    size_t pos_end_i = strtoul(pos_end, NULL, 10);
                    result = markdown_link(server_doc, server_doc->version, pos_start_i, pos_end_i, link);
                }
            }
            else {
                result = -4;
            }
            char *stat = "UNKNOWN_ERROR";
            if (result == -1) stat = "INVALID_CURSOR_POS";
            else if (result == -2) stat = "DELETED_POSITION";
            else if (result == -3) stat = "OUTDATED_VERSION";
            else if (result == -4) stat = "UNAUTHORIZED";
            pthread_mutex_unlock(&doc_mtx);
            char resp[512];
            int len;

            if (result == 0) {
                printf("username: %s, line: %s\n", username, line);
                len = snprintf(resp, sizeof(resp), "%s %s %s %s\n", "EDIT", username, line, "SUCCESS");
                updated = 1;
            }
            else {
                printf("username: %s, line: %s\n", username, line);
                len = snprintf(resp, sizeof(resp), "%s %s %s %s %s\n", "EDIT", username, line, "Rejected", stat);
            }

            if (len > 0 && len < (int)sizeof(resp)) {
                char *copy = malloc(len + 1);
                if (!copy) {
                    continue;
                }
                memcpy(copy, resp, len);
                copy[len] = '\0';
                pending_responses[pending_count] = copy;
                pending_lens[pending_count] = len;
                pending_count++;
            }
        }
        if (updated) {
            updated = 0;
            pthread_mutex_lock(&doc_mtx);
            markdown_increment_version(server_doc);
            pthread_mutex_unlock(&doc_mtx);
        }
        for (size_t i = 0; i < n; i++) {
            free(arr[i]->line);
            free(arr[i]);
        }
        free(arr);
    }
    return NULL;
}

void *client_thread(void *arg) {
    (void)arg;
    pid_t client_pid = (pid_t)(intptr_t)arg;
    Client *me = NULL;
    char fifo_name_c2s[64];
    char fifo_name_s2c[64];
    sprintf(fifo_name_c2s, "./FIFO_C2S_%d", client_pid);
    sprintf(fifo_name_s2c, "./FIFO_S2C_%d", client_pid);

    unlink(fifo_name_c2s);
    unlink(fifo_name_s2c);

    int ret;
    ret = mkfifo(fifo_name_c2s, 0666);
    if (ret < 0 && errno == EPERM) {
        sprintf(fifo_name_c2s, "/tmp/FIFO_C2S_%d", client_pid);
        if (mkfifo(fifo_name_c2s, 0666) < 0) {
            perror("mkfifo fallback");
            return NULL;
        }
    } else if (ret < 0) {
        perror("mkfifo");
        return NULL;
    }

    ret = mkfifo(fifo_name_s2c, 0666);
    if (ret < 0 && errno == EPERM) {
        sprintf(fifo_name_s2c, "/tmp/FIFO_S2C_%d", client_pid);
        if (mkfifo(fifo_name_s2c, 0666) < 0) {
            perror("mkfifo fallback");
            return NULL;
        }
    } else if (ret < 0) {
        perror("mkfifo");
        return NULL;
    }

    kill(client_pid, SIGRTMIN + 1);

    int fifo_fd_c2s = open(fifo_name_c2s, O_RDONLY); 
    int fifo_fd_s2c = open(fifo_name_s2c, O_WRONLY);
    if (fifo_fd_c2s == -1 || fifo_fd_s2c == -1) {
        goto clean;
    }

    char buffer[1024];
    ssize_t bytes_read = read(fifo_fd_c2s, buffer, sizeof(buffer));

    if (bytes_read <= 0) { goto clean; }
    buffer[bytes_read] = '\0';

    me = malloc(sizeof(*me));
    if (!me) { goto clean; }
    me->fifo_fd_s2c = fifo_fd_s2c;
    me->fifo_fd_c2s = fifo_fd_c2s;
    strncpy(me->fifo_name_c2s, fifo_name_c2s, sizeof(me->fifo_name_c2s)-1);
    me->fifo_name_c2s[sizeof(me->fifo_name_c2s)-1] = '\0';
    strncpy(me->fifo_name_s2c, fifo_name_s2c, sizeof(me->fifo_name_s2c)-1);
    me->fifo_name_s2c[sizeof(me->fifo_name_s2c)-1] = '\0';
    strncpy(me->username, buffer, sizeof(me->username)-1);
    me->username[sizeof(me->username)-1] = '\0';
    me->quit = 0;
    register_client(me);

    char username[20];
    strncpy(username, buffer, sizeof(username));
    username[strcspn(username, "\n")] = '\0';

    FILE *role_file = fopen(rolefile, "r");
    if (role_file == NULL) {
        goto clean;
    }
    char line[64];
    char uname[32];
    char role[32];
    while (fgets(line, sizeof(line), role_file)) {
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%19s %19s", uname, role) == 2) {
            if (strcmp(uname, username) == 0) {
                if (strcmp(role, "write") != 0 && strcmp(role, "read") != 0) break;
                role[strlen(role)] = '\n';
                ssize_t w = write(fifo_fd_s2c, role, strlen(role));
                if (w < 0) {
                    goto clean;
                }
                if (strcmp(role, "write\n") == 0) {
                    me->role = 1;
                }
                else if (strcmp(role, "read\n") == 0) {
                    me->role = 0;
                }

                char header[256] = {0};
                char *content = NULL;
                pthread_mutex_lock(&doc_mtx);
                int len = snprintf(header, sizeof(header), 
                                    "%" PRIu64 "\n%zu\n", 
                                    server_doc->version, server_doc->flattened_length);
                if (len < 0 || write(fifo_fd_s2c, header, (size_t)len) != len) {
                    fclose(role_file);
                    goto clean;
                }

                content = server_doc->flattened;
                if (!content) {
                    fclose(role_file);
                    goto clean;
                }

                size_t send_size = server_doc->flattened_length;
                char *ptr = content;
                while (send_size > 0) {
                    ssize_t w = write(fifo_fd_s2c, ptr, send_size);
                    if (w <= 0) {
                        goto clean;
                    }
                    ptr += w;
                    send_size -= w;
                }
                fclose(role_file);
                pthread_mutex_unlock(&doc_mtx);
                pthread_amount++;
                char buffer[256];
                while(!stop_requested && !me->quit && (bytes_read = read(fifo_fd_c2s, buffer, sizeof(buffer)-1)) > 0) {
                    buffer[bytes_read] = '\0';
                    enqueue_command(buffer, me);
                }
                pthread_amount--;
                goto clean;
            }
        }

    }
    fclose(role_file);
    const char* reject = "Reject UNAUTHORISED\n";
    ssize_t w = write(fifo_fd_s2c, reject, strlen(reject));
    if (w < 0) {
        goto clean;
    }
    sleep(1);
    goto clean;

    clean:
        if (fifo_fd_c2s >= 0) close(fifo_fd_c2s);
        if (fifo_fd_s2c >= 0) close(fifo_fd_s2c);
        if (me) unregister_client(me);
        unlink(fifo_name_c2s);
        unlink(fifo_name_s2c);
        return NULL;
    }

void handle_signal(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)context;
    pid_t client_pid = info->si_pid;
    pthread_t thread;
    if (pthread_create(&thread, NULL, client_thread, (void *)(intptr_t)client_pid) == 0) {
        pthread_detach(thread);
    }
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        fprintf(stdout, "Usage: %s <TIME_INTERVAL>\n", argv[0]);
        fflush(stdout);
        return 1;
    }

    char *endptr;
    long time_interval = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || time_interval <= 0) {
        fprintf(stdout, "Invalid time interval\n");
        fflush(stdout);
        return 1;
    }

    server_doc = markdown_init();
    if (server_doc == NULL) {
        fprintf(stdout, "Failed to initialize document\n");
        fflush(stdout);
        return 1;
    }

    pid_t pid = getpid();
    fprintf(stdout, "Server PID: %d\n", pid);
    fflush(stdout);

    struct sigaction sa_term = { .sa_handler = cleanup_handler };
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);

    pthread_t input_thread;
    pthread_t edit_thread;
    if (pthread_create(&input_thread, NULL, stdin_thread, NULL) != 0 ||
        pthread_create(&edit_thread, NULL, edit_document, (void *)time_interval) != 0) {
        return 1;
    }
    
    struct sigaction sa_rt = {0};
    sigemptyset(&sa_rt.sa_mask);
    sa_rt.sa_flags = SA_SIGINFO;
    sa_rt.sa_sigaction = handle_signal;
    if (sigaction(SIGRTMIN, &sa_rt, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    while (!stop_requested) {
        pause();
    }
    
    pthread_cancel(input_thread);
    pthread_cancel(edit_thread);
    pthread_join(input_thread, NULL);
    pthread_join(edit_thread, NULL);

    FILE* doc_file = fopen("doc.md", "w");
    if (doc_file) {
        char* content = markdown_flatten(server_doc);
        fwrite(content, 1, strlen(content), doc_file);
        if (content) free(content);
        fclose(doc_file);
    }

    markdown_free(server_doc);
    pthread_mutex_destroy(&clients_mtx);
    pthread_mutex_destroy(&cmd_mtx);
    pthread_mutex_destroy(&doc_mtx);

    return 0;
}
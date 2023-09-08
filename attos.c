#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

char* base_path = ".";
int port = 8080;
int show_listing = 1;

int server_sock = -1;

char* shift_arg(int* argc, char*** argv) {
    if (*argc <= 0) {
        fprintf(stderr, "[ERRO] Trying to shift non existing args\n");
        exit(1);
    }
    char* arg = **argv;
    (*argc)--;
    (*argv)++;
    return arg;
}

void print_usage(FILE* fd, const char* program_name) {
    fprintf(fd, "USAGE: %s [<FLAGS>] <PATH>\n", program_name);
    fprintf(fd, "PATH:\n");
    fprintf(fd, "  path to the directory you want to serve\n");
    fprintf(fd, "FLAGS:\n");
    fprintf(fd, "  -h | -? | --help           : Show this message on stdout\n");
    fprintf(fd, "  -p <PORT> | --port <PORT>  : Set port to listen on\n");
    fprintf(fd, "  -L | --no-listing          : Disable showing dir listing\n");
    fprintf(fd, "                               (show index.html instead)\n");
}

void onexit(void) {
    if (server_sock != -1) close(server_sock);
}

void sighandler(int signum) { exit(1); }

char* parse_word(char* inp, char* res) {
    while (isspace(*inp)) inp++;
    while (!isspace(*inp) && *inp) {
        if (res) *res++ = *inp;
        inp++;
    }
    return inp + (*inp != 0);
}

#define HTTP_VERSION "HTTP/1.1"
#define STATUS_500 "500 Internal server error"
#define STATUS_405 "405 Method Not Allowed"
#define STATUS_403 "403 Forbidden"
#define STATUS_404 "404 Not Found"
#define STATUS_200 "200 OK"
#define STATUS_303 "303 See Other"
#define STANDARD_HEADERS           \
    "Server: Atto Http Server\r\n" \
    "Access-Control-Allow-Origin: *\r\n"

void send_ok_response(int fd, const char* data, size_t data_size,
                      const char* mimetype) {
    // clang-format off
    const char* header_template =
        HTTP_VERSION " "
        STATUS_200 "\r\n"
        STANDARD_HEADERS
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "\r\n";
    // clang-format on
    char header[1024];

    snprintf(header, 1024, header_template, mimetype, data_size);

    send(fd, header, strlen(header), 0);
    send(fd, data, data_size, 0);
}

void send_not_found(int fd) {
    const char* header_template =
        HTTP_VERSION " " STATUS_404 "\r\n" STANDARD_HEADERS "\r\n";

    char header[1024];
    snprintf(header, 1024, "%s", header_template);

    send(fd, header, strlen(header), 0);
}

void send_forbidden(int fd) {
    const char* header_template =
        HTTP_VERSION " " STATUS_403 "\r\n" STANDARD_HEADERS "\r\n";

    char header[1024];
    snprintf(header, 1024, "%s", header_template);

    send(fd, header, strlen(header), 0);
}

void send_method_not_allowed(int fd) {
    const char* header_template =
        HTTP_VERSION " " STATUS_405 "\r\n" STANDARD_HEADERS "\r\n";

    char header[1024];
    snprintf(header, 1024, "%s", header_template);

    send(fd, header, strlen(header), 0);
}

void send_internal_error(int fd) {
    const char* header_template =
        HTTP_VERSION " " STATUS_500 "\r\n" STANDARD_HEADERS "\r\n";

    char header[1024];
    snprintf(header, 1024, "%s", header_template);

    send(fd, header, strlen(header), 0);
}

void send_redirect(int fd, const char* url) {
    // clang-format off
    const char* header_template =
        HTTP_VERSION " " STATUS_303 "\r\n"
        STANDARD_HEADERS 
        "Location: %s\r\n";
    // clang-format on

    char header[1024];
    snprintf(header, 1024, header_template, url);

    send(fd, header, strlen(header), 0);
}

void send_html(int fd, const char* html) {
    send_ok_response(fd, html, strlen(html), "text/html");
}

struct mimetype_def {
    char *extension, *mimetype;
} known_types[] = {
    //clang-format off
    {"html", "text/html"},
    {"css",  "text/css"},
    {"js",   "text/javascript"},
    {"png",  "image/png"},
    {"jpg",  "image/jpg"},
    {"bmp",  "image/bmp"},
    //clang-format on
};

#define known_types_count (sizeof(known_types) / sizeof(known_types[0]))

char* get_mimetype(const char* name) {
    int ext_start = strlen(name) - 1;

    while (ext_start >= 0 && name[ext_start] != '.') ext_start--;

    ext_start++;

    for (size_t i = 0; i < known_types_count; i++) {
        if (strcmp(&name[ext_start], known_types[i].extension) == 0) {
            return known_types[i].mimetype;
        }
    }

    return "text/plain";
}

void send_file(int fd, const char* name) {
    FILE* file = fopen(name, "r");

    fseek(file, 0, SEEK_END);
    size_t data_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char data[data_size + 1];
    fread(data, data_size, 1, file);

    fclose(file);

    send_ok_response(fd, data, data_size, get_mimetype(name));
}

void send_directory_index(int fd, const char* name) {
    char buffer[64000] = {0};
    char* cursor = buffer;
    size_t size_left = sizeof(buffer);

    {
        // clang-format off
        int bytes_written = snprintf(cursor, size_left, 
                "<head>"
                "<meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
                "<title>Index of %s</title>"
                "</head>"
                "<body>"
                "<h1>Index of %s</h1>"
                "<ul>"
                , name
                , name
        );
        // clang-format on
        cursor += bytes_written;
        size_left -= bytes_written;
    }

    DIR* dir = opendir(name);
    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        // clang-format off
        int bytes_written = snprintf(cursor, size_left, 
                "<li><a href=\"./%s%c\">%s%c</a></li>"
                , entry->d_name
                , (entry->d_type == DT_DIR ? '/' : ' ')
                , entry->d_name
                , (entry->d_type == DT_DIR ? '/' : ' ')
        );
        // clang-format on
        cursor += bytes_written;
        size_left -= bytes_written;
    }

    {
        // clang-format off
        int bytes_written = snprintf(cursor, size_left, 
                "</ul>"
                "</body>"
        );
        // clang-format on
        cursor += bytes_written;
        size_left -= bytes_written;
    }

    closedir(dir);
    send_html(fd, buffer);
}

void handle_connection(int client_sock) {
    char buffer[1024] = {0};
    char* cursor = buffer;

    if (recv(client_sock, buffer, sizeof(buffer), 0) <= 0) {
        printf("[WARN] Failed to receive data from client: %s\n",
               strerror(errno));
        return;
    }

    char method[16] = {0};
    cursor = parse_word(cursor, method);

    char url[256] = {0};
    cursor = parse_word(cursor, url);

    // ignore version
    cursor = parse_word(cursor, 0);

    if (strcmp(method, "GET") == 0) {
        printf("[INFO]: Request: %s %s\n", method, url);

        int base_path_len = strlen(base_path);
        int url_len = strlen(url);
        char resource_name[base_path_len + url_len + sizeof("index.html") + 1];
        strcpy(resource_name, base_path);
        strcat(resource_name, url);

        struct stat st;
        if (stat(resource_name, &st)) {
            if (errno == ENOENT) {
                printf("[INFO]: Sending not found `%s`\n", resource_name);
                send_not_found(client_sock);
            } else {
                fprintf(stderr, "[ERRO] Failed to stat(\"%s\"): %s\n",
                        resource_name, strerror(errno));
                send_internal_error(client_sock);
            }
            return;
        }

        switch (st.st_mode & S_IFMT) {
            case S_IFDIR: {
                if (resource_name[strlen(resource_name) - 1] != '/') {
                    printf("[INFO]: Redirecting to `%s/`\n", resource_name);
                    char resource_name_fixed[strlen(resource_name) + 1 + 1];
                    strcpy(resource_name_fixed, resource_name);
                    strcat(resource_name_fixed, "/");
                    send_redirect(client_sock, resource_name_fixed);
                } else {
                    if (show_listing) {
                        printf("[INFO]: Sending index of `%s`\n",
                               resource_name);
                        send_directory_index(client_sock, resource_name);
                    } else {
                        strcat(resource_name, "index.html");
                        if (stat(resource_name, &st)) {
                            if (errno == ENOENT) {
                                printf("[INFO]: Sending not found `%s`\n",
                                       resource_name);
                                send_not_found(client_sock);
                            } else {
                                fprintf(stderr,
                                        "[ERRO] Failed to stat(\"%s\"): %s\n",
                                        resource_name, strerror(errno));
                                send_internal_error(client_sock);
                            }
                            return;
                        }
                        printf("[INFO]: Sending index file `%s`\n",
                               resource_name);
                        send_file(client_sock, resource_name);
                    }
                }
            } break;
            case S_IFREG: {
                printf("[INFO]: Sending file `%s`\n", resource_name);
                send_file(client_sock, resource_name);
            } break;
            default: {
                send_forbidden(client_sock);
            } break;
        }
    } else {
        printf("[INFO]: Bad method: %s %s\n", method, url);
        send_method_not_allowed(client_sock);
    }
}

int main(int argc, char** argv) {
    char* program_name = shift_arg(&argc, &argv);
    while (argc) {
        char* arg = shift_arg(&argc, &argv);
        if (arg[0] == '-') {
            if (strcmp(arg, "-h") == 0 || strcmp(arg, "-?") == 0 ||
                strcmp(arg, "--help") == 0) {
                print_usage(stdout, program_name);
                exit(0);
            } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
                if (argc == 0) {
                    fprintf(stderr,
                            "[ERRO]: Expected argument specifying port");
                    exit(0);
                }

                char* port_s = shift_arg(&argc, &argv);

                port = atoi(port_s);
            } else if (strcmp(arg, "-L") == 0 ||
                       strcmp(arg, "--no-listing") == 0) {
                show_listing = 0;
            } else {
                fprintf(stderr, "[ERRO]: Unknown flag: `%s`\n", arg);
                fprintf(stderr, "[INFO]: Try `%s --help`\n", program_name);
                exit(1);
            }
        } else {
            base_path = arg;
        }
    }

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "[ERRO] Failed to create a socket: %s\n",
                strerror(errno));
        exit(1);
    }

    atexit(onexit);
    signal(SIGINT, sighandler);

    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEPORT, &(int){1},
                   sizeof(int))) {
        fprintf(stderr, "[ERRO] Failed to setsockopt(SO_REUSEPORT): %s\n",
                strerror(errno));
        exit(1);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_addr = (struct in_addr){0};
    server_addr.sin_port = htons(port);
    server_addr.sin_family = AF_INET;

    if (bind(server_sock, (struct sockaddr*)&server_addr,
             sizeof(server_addr))) {
        fprintf(stderr, "[ERRO] Failed to bind socket to addr: %s\n",
                strerror(errno));
        exit(1);
    }

    if (listen(server_sock, 1)) {
        fprintf(stderr, "[ERRO] Failed to start listening on socket: %s\n",
                strerror(errno));
        exit(1);
    }

    printf("[INFO]: Serving %s on http://localhost:%04d/\n", base_path, port);

    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        handle_connection(client_sock);
        close(client_sock);
    }
}

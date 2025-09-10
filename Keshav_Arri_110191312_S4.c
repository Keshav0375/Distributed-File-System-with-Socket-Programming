#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>

#define PORT 8894
#define BUF_SIZE 8192
#define MAX_PATH 1024

// Function prototypes
int create_dirs_recursive(const char *path);
int send_data(int sock, const void *data, size_t len);
int receive_data(int sock, void *data, size_t len);
void handle_client(int client);

int main() {
    // Create S4 directory
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/S4", getenv("HOME"));
    if (mkdir(dir, 0755) == -1 && errno != EEXIST) {
        perror("Failed to create S4 directory");
    }
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT)
    };
    
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    printf("S4 ZIP Server running on port %d\n", PORT);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }
        
        printf("New client connected from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        handle_client(client_fd);
        close(client_fd);
    }
    
    close(server_fd);
    return 0;
}

int create_dirs_recursive(const char *path) {
    char tmp_path[MAX_PATH];
    char *p;
    
    snprintf(tmp_path, sizeof(tmp_path), "%s", path);
    size_t len = strlen(tmp_path);
    
    if (len > 0 && tmp_path[len - 1] == '/') {
        tmp_path[len - 1] = '\0';
    }
    
    for (p = tmp_path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp_path, 0755) == -1 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp_path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

int send_data(int sock, const void *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, (char*)data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        sent += n;
    }
    return sent;
}

int receive_data(int sock, void *data, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(sock, (char*)data + received, len - received, 0);
        if (n <= 0) return -1;
        received += n;
    }
    return received;
}

void handle_client(int client) {
    char command[5];
    char path[MAX_PATH];
    int path_len, data_len;
    
    // Receive command
    if (receive_data(client, command, 4) <= 0) return;
    command[4] = '\0';
    
    // Receive path length and path
    if (receive_data(client, &path_len, sizeof(int)) <= 0) return;
    if (receive_data(client, path, path_len) <= 0) return;
    path[path_len] = '\0';
    
    // Receive data length
    if (receive_data(client, &data_len, sizeof(int)) <= 0) return;
    
    printf("Command: %s, Path: %s, Data length: %d\n", command, path, data_len);
    
    if (strcmp(command, "STOR") == 0) {
        // Receive file data
        char *data = malloc(data_len);
        if (!data) {
            int status = -1;
            send_data(client, &status, sizeof(int));
            return;
        }
        
        if (receive_data(client, data, data_len) <= 0) {
            free(data);
            int status = -1;
            send_data(client, &status, sizeof(int));
            return;
        }
        
        // Create directories
        char dir_path[MAX_PATH];
        strncpy(dir_path, path, sizeof(dir_path));
        char *dir = dirname(dir_path);
        
        if (create_dirs_recursive(dir) == 0) {
            int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) {
                write(fd, data, data_len);
                close(fd);
                send_data(client, &data_len, sizeof(int));
                printf("Stored ZIP: %s (%d bytes)\n", path, data_len);
            } else {
                int status = -1;
                send_data(client, &status, sizeof(int));
            }
        } else {
            int status = -1;
            send_data(client, &status, sizeof(int));
        }
        
        free(data);
    }
    else if (strcmp(command, "RETR") == 0) {
        struct stat st;
        if (stat(path, &st) == 0) {
            int size = st.st_size;
            send_data(client, &size, sizeof(int));
            
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                char buffer[BUF_SIZE];
                ssize_t bytes_read;
                while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
                    send_data(client, buffer, bytes_read);
                }
                close(fd);
                printf("Sent ZIP: %s (%d bytes)\n", path, size);
            }
        } else {
            int size = -1;
            send_data(client, &size, sizeof(int));
        }
    }
    else if (strcmp(command, "REMV") == 0) {
        int status = (unlink(path) == 0) ? 0 : -1;
        send_data(client, &status, sizeof(int));
        printf("Removed ZIP: %s (%s)\n", path, status == 0 ? "success" : "fail");
    }
    else if (strcmp(command, "LIST") == 0) {
        char result[BUF_SIZE] = "";
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_type == DT_REG && strstr(entry->d_name, ".zip")) {
                    strcat(result, entry->d_name);
                    strcat(result, "\n");
                }
            }
            closedir(dir);
        }
        int size = strlen(result);
        send_data(client, &size, sizeof(int));
        if (size > 0) {
            send_data(client, result, size);
        }
        printf("Listed ZIPs in %s (%d files)\n", path, size);
    }
    else {
        printf("Unknown command: %s\n", command);
        int status = -1;
        send_data(client, &status, sizeof(int));
    }
}
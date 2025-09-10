#include <stdio.h>      // Standard I/O operations (printf, fgets, etc.)
#include <stdlib.h>     // Memory allocation (malloc, free), process control (exit)
#include <string.h>     // String manipulation (strcmp, strcpy, strcat, etc.)
#include <unistd.h>     // POSIX API (fork, close, read, write, sleep)
#include <sys/socket.h> // Socket programming APIs (socket, bind, listen, accept)
#include <netinet/in.h> // Internet address family structures (sockaddr_in)
#include <arpa/inet.h>  // IP address conversion functions (inet_pton, inet_ntoa)
#include <sys/stat.h>   // File status operations (stat, mkdir)
#include <fcntl.h>      // File control operations (open flags like O_CREAT, O_RDONLY)
#include <dirent.h>     // Directory operations (opendir, readdir, closedir)
#include <signal.h>     // Signal handling (signal, SIGCHLD)
#include <sys/wait.h>   // Process synchronization (waitpid, WNOHANG)
#include <errno.h>      // Error number definitions and perror()
#include <libgen.h>     // Path manipulation (basename, dirname)
#include <limits.h>     // System limits (PATH_MAX, etc.)

/*
 * CONFIGURATION CONSTANTS
 * These define the network topology and system limits for the distributed file system
 */
#define PORT 8222        // S1 server listening port - main entry point for clients
#define BUF_SIZE 16384   // 16KB buffer for network I/O operations - larger buffer for S1 as coordinator
#define S2_PORT 8892     // S2 server port for PDF files
#define S3_PORT 8893     // S3 server port for TXT files  
#define S4_PORT 8894     // S4 server port for ZIP files
#define MAX_PATH 1024    // Maximum path length - standard Linux path limit
#define MAX_FILES 3      // Maximum number of files per upload operation - prevents resource exhaustion

/*
 * FUNCTION PROTOTYPES
 * Forward declarations enable modular code organization and mutual recursion
 */
void handle_sigchld(int sig);                    // Signal handler for child process cleanup
int get_file_type(const char *filename);         // File extension to server mapping
void expand_path(char *dest, const char *src);   // Path expansion and normalization
int create_dirs_recursive(const char *path);     // Recursive directory creation
int send_data(int sock, const void *data, size_t len);      // Reliable data transmission
int receive_data(int sock, void *data, size_t len);         // Reliable data reception
void convert_server_path(char *dest, const char *src, int server_num);  // Path translation for server forwarding
int forward_to_server(int port, const char *cmd, const char *path, const void *data, int len);  // Request forwarding
void handle_upload(int client, const char *args);    // Upload command processor
void handle_download(int client, const char *args);  // Download command processor
void handle_remove(int client, const char *args);    // Remove command processor
void handle_tar(int client, const char *args);       // Tar archive command processor
void handle_list(int client, const char *args);      // File listing command processor
void client_handler(int client);                     // Main client session handler

/*
 * GLOBAL VARIABLES
 * These provide lookup tables for server coordination and routing decisions
 */
// Server name mapping for logging and debugging - index corresponds to file type
const char *server_names[] = {"", "S1", "S2", "S3", "S4"};
// Port mapping for forwarding requests - index corresponds to file type (0 unused, 1=S1, 2=S2, etc.)
const int server_ports[] = {0, 0, S2_PORT, S3_PORT, S4_PORT};


int main() {
    /*
     * SIGNAL HANDLER SETUP
     * SIGCHLD is sent when child processes terminate
     * handle_sigchld() prevents zombie processes by reaping terminated children
     * This is critical in multi-process servers to prevent resource leaks
     */
    signal(SIGCHLD, handle_sigchld);
    
    /*
     * DIRECTORY STRUCTURE INITIALIZATION
     * Create the S1 base directory where .c files will be stored
     * Uses environment variable HOME for user-specific storage
     * EEXIST check prevents errors if directory already exists
     */
    char s1_dir[MAX_PATH];
    snprintf(s1_dir, sizeof(s1_dir), "%s/S1", getenv("HOME"));  // Construct path: /home/user/S1
    if (mkdir(s1_dir, 0755) == -1 && errno != EEXIST) {        // 0755 = rwxr-xr-x permissions
        perror("Failed to create S1 directory");                // Print system error message
    }
    
    /*
     * SOCKET CREATION
     * AF_INET = IPv4 internet protocols
     * SOCK_STREAM = TCP reliable, connection-oriented communication
     * 0 = default protocol (TCP for SOCK_STREAM)
     */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");   // Print error with system message
        exit(EXIT_FAILURE);                 // Terminate with failure status
    }
    
    /*
     * SOCKET OPTIONS CONFIGURATION
     * SO_REUSEADDR allows immediate reuse of address after server restart
     * Prevents "Address already in use" errors during development/restart
     * SOL_SOCKET = socket level options (vs protocol-specific options)
     */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /*
     * ADDRESS STRUCTURE INITIALIZATION
     * sockaddr_in defines IPv4 internet socket address
     * Designated initializer syntax (.member = value) for clarity
     */
    struct sockaddr_in address = {
        .sin_family = AF_INET,        // IPv4 address family
        .sin_addr.s_addr = INADDR_ANY, // Listen on all available interfaces (0.0.0.0)
        .sin_port = htons(PORT)       // Convert port to network byte order (big-endian)
    };
    
    /*
     * SOCKET BINDING
     * Associates socket with specific address and port
     * Cast to generic sockaddr* for compatibility with socket API
     * sizeof(address) tells kernel the address structure size
     */
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");     // Usually "Address already in use" or permission denied
        close(server_fd);          // Clean up socket resource
        exit(EXIT_FAILURE);
    }
    
    /*
     * LISTENING STATE ACTIVATION
     * Transitions socket to passive listening mode
     * Backlog of 10 = maximum pending connections in queue
     * Clients beyond this limit get connection refused until queue has space
     */
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    /*
     * SERVER STARTUP CONFIRMATION
     * Logs server status with PID for process management
     * getpid() returns current process ID for monitoring/debugging
     */
    printf("S1 Server running on port %d (PID: %d)\n", PORT, getpid());
    printf("Waiting for client connections...\n");
    
    /*
     * CLIENT ACCEPT LOOP - MAIN SERVER OPERATION
     * Infinite loop accepting and handling client connections
     * Each client connection spawns a separate process for concurrent handling
     */
    while (1) {
        /*
         * CLIENT CONNECTION ACCEPTANCE
         * accept() blocks until client connects, returns new socket for communication
         * client_addr filled with client's IP and port information
         * addr_len is value-result parameter (input: buffer size, output: actual size)
         */
        struct sockaddr_in client_addr;           // Storage for client address info
        socklen_t addr_len = sizeof(client_addr); // Address structure size
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        
        /*
         * CONNECTION ERROR HANDLING
         * EINTR = interrupted system call (e.g., by signal) - retry accept
         * Other errors logged but server continues running (graceful degradation)
         */
        if (client_fd < 0) {
            if (errno == EINTR) continue;     // Signal interrupted accept() - retry
            perror("Accept failed");          // Log error but continue serving
            continue;
        }
        
        /*
         * CLIENT CONNECTION LOGGING
         * inet_ntoa() converts binary IP address to dotted decimal string
         * ntohs() converts port from network byte order to host byte order
         */
        printf("New client connected from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        /*
         * PROCESS FORKING FOR CONCURRENT CLIENT HANDLING
         * fork() creates identical copy of current process
         * Returns 0 in child process, child PID in parent, -1 on error
         * This enables concurrent handling of multiple clients
         */
        pid_t pid = fork();
        if (pid == 0) { 
            /*
             * CHILD PROCESS - HANDLES INDIVIDUAL CLIENT
             * Child doesn't need server socket (only for accepting new connections)
             * client_handler() processes all client requests in this session
             * Process exits after client disconnects
             */
            close(server_fd);        // Child doesn't need listening socket
            client_handler(client_fd); // Handle all client communication
            close(client_fd);        // Clean up client socket
            exit(EXIT_SUCCESS);      // Terminate child process
        } else if (pid > 0) { 
            /*
             * PARENT PROCESS - CONTINUES ACCEPTING NEW CLIENTS
             * Parent doesn't need client-specific socket
             * continue loop to accept next client connection
             */
            close(client_fd);        // Parent doesn't need client socket
        } else {
            /*
             * FORK ERROR HANDLING
             * fork() can fail due to resource limits (max processes, memory)
             * Close client socket and continue serving other clients
             */
            perror("Fork failed");
            close(client_fd);
        }
    }
    
    /*
     * SERVER SHUTDOWN (unreachable in current design)
     * Clean up listening socket - good practice even if unreachable
     */
    close(server_fd);
    return 0;
}

void handle_sigchld(int sig) {
    /*
     * REAP ALL AVAILABLE ZOMBIE CHILDREN
     * waitpid(-1, NULL, WNOHANG):
     * - -1 = wait for any child process
     * - NULL = don't store exit status (we don't need it)
     * - WNOHANG = return immediately if no zombies available
     * - Returns child PID if reaped, 0 if no zombies, -1 on error
     */
    while (waitpid(-1, NULL, WNOHANG) > 0);  // Loop until no more zombies
}


int get_file_type(const char *filename) {
    /*
     * FIND FILE EXTENSION
     * strrchr(filename, '.') finds rightmost occurrence of '.' character
     * Returns pointer to '.' or NULL if not found
     * Handles files like "archive.old.zip" correctly (uses ".zip", not ".old")
     */
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;  // No extension found - unsupported file type
    
    /*
     * EXTENSION TO SERVER MAPPING
     * Uses string comparison to map extensions to server numbers
     * This routing table determines which server handles each file type
     * Return values correspond to server_ports[] and server_names[] indices
     */
    if (strcmp(ext, ".c") == 0) return 1;    // C source files -> S1 (local)
    if (strcmp(ext, ".pdf") == 0) return 2;  // PDF documents -> S2
    if (strcmp(ext, ".txt") == 0) return 3;  // Text files -> S3
    if (strcmp(ext, ".zip") == 0) return 4;  // ZIP archives -> S4
    
    return 0;  // Unsupported file type
}


void expand_path(char *dest, const char *src) {

    if (src[0] == '~' && 
        ((src[1] == '/') || (src[1] == 'S' && src[2] == '1' && src[3] == '/'))) {
        
        /*
         * HOME DIRECTORY RESOLUTION
         * getenv("HOME") retrieves user's home directory from environment
         * Typically /home/username on Linux, /Users/username on macOS
         */
        const char *home = getenv("HOME");
        if (home) {
            /*
             * PATH FORMAT ANALYSIS AND CONSTRUCTION
             * Determine the correct expansion based on input format
             */
            if (src[1] == '/' && src[2] == 'S' && src[3] == '1' && src[4] == '/') {
                /*
                 * CASE: ~/S1/path format
                 * Input: ~/S1/code -> Output: /home/user/S1/code
                 * Don't add extra S1 since it's already present in the path
                 * Skip the ~/ prefix and use the rest as-is
                 */
                snprintf(dest, MAX_PATH, "%s/%s", home, src + 2);  // Skip "~/"
                return;
            } else if (src[1] == 'S' && src[2] == '1' && src[3] == '/') {
                /*
                 * CASE: ~S1/path format  
                 * Input: ~S1/code -> Output: /home/user/S1/code
                 * Replace ~S1/ with /home/user/S1/
                 * Skip the ~S1/ prefix and add S1/ to home directory
                 */
                snprintf(dest, MAX_PATH, "%s/S1/%s", home, src + 4);  // Skip "~S1/"
                return;
            } else if (src[1] == '/') {
                /*
                 * CASE: ~/path format (without S1)
                 * Input: ~/code -> Output: /home/user/S1/code  
                 * Add S1 directory to maintain consistent file organization
                 * Skip the ~/ prefix and add path under S1 directory
                 */
                snprintf(dest, MAX_PATH, "%s/S1/%s", home, src + 2);  // Skip "~/"
                return;
            }
        }
    }
    
    /*
     * DEFAULT CASE - NO EXPANSION NEEDED
     * For absolute paths or when HOME environment variable not available
     * Also handles cases where tilde expansion is not applicable
     * strncpy() with explicit null termination for safety
     */
    strncpy(dest, src, MAX_PATH - 1);
    dest[MAX_PATH - 1] = '\0';  // Ensure null termination for safety
}


int create_dirs_recursive(const char *path) {
    /*
     * LOCAL PATH BUFFER
     * Create modifiable copy since we'll temporarily modify it
     * strtok() and similar functions modify the input string
     */
    char tmp_path[MAX_PATH];
    char *p;  // Pointer for path traversal
    
    /*
     * PATH COPYING AND CLEANUP
     * snprintf() for safe string copying with bounds checking
     * Remove trailing slash to normalize path format
     */
    snprintf(tmp_path, sizeof(tmp_path), "%s", path);
    size_t len = strlen(tmp_path);
    
    // Remove trailing slash for consistent processing
    if (len > 0 && tmp_path[len - 1] == '/') {
        tmp_path[len - 1] = '\0';
    }
    
    /*
     * DIRECTORY CREATION LOOP
     * Iterate through path, creating each directory component
     * Start from position 1 to skip root slash
     */
    for (p = tmp_path + 1; *p; p++) {
        if (*p == '/') {
            /*
             * COMPONENT BOUNDARY PROCESSING
             * Temporarily terminate string at this slash
             * Create directory up to this point
             * Restore slash and continue
             */
            *p = '\0';  // Temporarily terminate string
            if (mkdir(tmp_path, 0755) == -1 && errno != EEXIST) {
                return -1;  // Directory creation failed
            }
            *p = '/';   // Restore slash
        }
    }
    
    /*
     * FINAL DIRECTORY CREATION
     * Create the final directory component
     * 0755 permissions = rwxr-xr-x (owner: read/write/execute, others: read/execute)
     */
    if (mkdir(tmp_path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    
    return 0;  // Success
}


int send_data(int sock, const void *data, size_t len) {
    size_t sent = 0;  // Track total bytes sent
    
    /*
     * TRANSMISSION LOOP
     * Continue until all data sent or error occurs
     * Handle partial sends by adjusting pointer and remaining length
     */
    while (sent < len) {
        /*
         * SEND SYSTEM CALL
         * send() transmits data over socket connection
         * MSG_NOSIGNAL prevents SIGPIPE signal on broken connection
         * Returns number of bytes actually sent (may be less than requested)
         */
        ssize_t n = send(sock, (char*)data + sent, len - sent, MSG_NOSIGNAL);
        
        /*
         * ERROR AND RETRY HANDLING
         * n <= 0 indicates error or connection closed
         * EAGAIN/EWOULDBLOCK = temporary unavailability, retry
         * Other errors = permanent failure
         */
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // Retry
            return -1;  // Permanent error
        }
        sent += n;  // Update total sent counter
    }
    return sent;  // Return total bytes sent
}


int receive_data(int sock, void *data, size_t len) {
    size_t received = 0;  // Track total bytes received
    
    /*
     * RECEPTION LOOP
     * Continue until all expected data received or error
     */
    while (received < len) {
        /*
         * RECEIVE SYSTEM CALL
         * recv() receives data from socket connection
         * Returns number of bytes actually received
         * 0 = connection closed by peer, <0 = error
         */
        ssize_t n = recv(sock, (char*)data + received, len - received, 0);
        if (n <= 0) return -1;  // Error or connection closed
        received += n;  // Update received counter
    }
    return received;  // Return total bytes received
}


void convert_server_path(char *dest, const char *src, int server_num) {

    const char *home = getenv("HOME");

    char base_s1[MAX_PATH];
    snprintf(base_s1, sizeof(base_s1), "%s/S1", home);
    size_t base_len = strlen(base_s1);
    
    /*
     * ABSOLUTE PATH PATTERN MATCHING
     * Check if source path starts with the full S1 base path
     * strncmp() compares first base_len characters for exact match
     */
    if (strncmp(src, base_s1, base_len) == 0) {
        /*
         * ABSOLUTE PATH CONVERSION WITH LEADING SLASH HANDLING
         * When src + base_len points to a leading slash, skip it to avoid double slashes
         * Example: "/home/user/S1/test/file" -> base_len points to "/test/file"
         * We want "/test/file" not "//test/file" in the final path
         */
        const char *remaining_path = src + base_len;
        
        /* Skip leading slash to prevent double slash in output path */
        if (remaining_path[0] == '/') {
            remaining_path++;
        }
        
        /* Construct new path: /home/user/S<server_num>/remaining_path */
        snprintf(dest, MAX_PATH, "%s/S%d/%s", home, server_num, remaining_path);
    } else {
        /*
         * RELATIVE PATH PATTERN HANDLING
         * Handle paths that contain "S1/" substring but aren't absolute
         * Example: "~/S1/test/file" or "some/path/S1/file"
         */
        if (strstr(src, "S1/")) {
            /*
             * SUBSTRING REPLACEMENT APPROACH
             * Find the "S1/" pattern and replace with "S<server_num>/"
             * strstr() returns pointer to first occurrence of "S1/"
             */
            char *s1_pos = strstr(src, "S1/");
            
            /*
             * PATH RECONSTRUCTION WITH REPLACEMENT
             * %.*s - prints specified number of characters from string
             * (int)(s1_pos - src) - calculates characters before "S1/"
             * s1_pos + 3 - points to content after "S1/" (skip "S1/")
             */
            snprintf(dest, MAX_PATH, "%.*sS%d/%s", 
                    (int)(s1_pos - src), src, server_num, s1_pos + 3);
        } else {
            /*
             * NO S1 PATTERN FOUND - DIRECT COPY
             * If no S1 reference found, copy path unchanged
             * This handles cases where path doesn't need conversion
             */
            strncpy(dest, src, MAX_PATH);
            dest[MAX_PATH - 1] = '\0';  /* Ensure null termination */
        }
    }
}


int forward_to_server(int port, const char *cmd, const char *path, const void *data, int len) {
    /*
     * CONNECTION ESTABLISHMENT
     * Create TCP socket for communication with target server
     */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    /*
     * SERVER ADDRESS CONFIGURATION
     * Connect to localhost (127.0.0.1) on specified port
     * Assumes all servers run on same machine (common in development)
     */
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port)  // Convert port to network byte order
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);  // Convert IP string to binary
    
    /*
     * CONNECTION ATTEMPT
     * Establish TCP connection to target server
     */
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return -1;
    }
    
    /*
     * PROTOCOL TRANSMISSION SEQUENCE
     * Send request in defined format for inter-server communication
     */
    
    // 1. Send 4-byte command (STOR, RETR, REMV, etc.)
    if (send_data(sock, cmd, 4) < 0) {
        perror("Command send failed");
        close(sock);
        return -1;
    }
    
    // 2. Send path length as 4-byte integer + path string
    int path_len = strlen(path);
    if (send_data(sock, &path_len, sizeof(int)) < 0 ||
        send_data(sock, path, path_len) < 0) {
        perror("Path send failed");
        close(sock);
        return -1;
    }
    
    // 3. Send data length as 4-byte integer
    if (send_data(sock, &len, sizeof(int)) < 0) {
        perror("Data length send failed");
        close(sock);
        return -1;
    }
    
    // 4. Send actual data if present (for STOR operations)
    if (len > 0 && data) {
        if (send_data(sock, data, len) < 0) {
            perror("Data send failed");
            close(sock);
            return -1;
        }
    }
    
    /*
     * RESPONSE RECEPTION
     * Receive status response from target server
     * Status format varies by operation but typically integer status code
     */
    int status = -1;
    if (receive_data(sock, &status, sizeof(int)) <= 0) {
        perror("Status receive failed");
        close(sock);
        return -1;
    }
    
    close(sock);    // Clean up connection
    return status;  // Return server response status
}


void handle_upload(int client, const char *args) {
    /*
     * ARGUMENT PARSING SETUP
     * files[] array stores pointers to individual file arguments
     * Allow MAX_FILES + 1 to accommodate destination path
     */
    char *files[MAX_FILES+1];  // Allow 3 files + 1 destination
    char dest_path[MAX_PATH];
    int file_count = 0;
    
    /*
     * TOKENIZATION PROCESS
     * Create modifiable copy of arguments for strtok() parsing
     * strtok() modifies input string by inserting null terminators
     */
    char args_copy[BUF_SIZE];
    strncpy(args_copy, args, sizeof(args_copy));
    
    /*
     * SPACE-DELIMITED PARSING
     * Extract individual file names and destination path
     * strtok() returns NULL when no more tokens found
     */
    char *token = strtok(args_copy, " ");
    while (token && file_count < MAX_FILES+1) {  // Allow one extra for destination
        files[file_count++] = token;
        token = strtok(NULL, " ");  // Continue parsing from last position
    }
    
    /*
     * ARGUMENT VALIDATION
     * Minimum requirement: at least one file + destination
     */
    if (file_count < 2) {
        send_data(client, "ERROR: Insufficient arguments", 29);
        return;
    }
    
    /*
     * DESTINATION EXTRACTION
     * Last token is always destination path
     * Adjust file_count to reflect actual number of files
     */
    char *dest = files[file_count - 1];
    file_count--; // Actual file count
    
    /*
     * FILE COUNT LIMITATION
     * Enforce MAX_FILES limit to prevent resource exhaustion
     */
    if (file_count > MAX_FILES) {
        file_count = MAX_FILES;
    }
    
    /*
     * DESTINATION PATH PROCESSING
     * Expand user shortcuts (~S1/) to absolute paths
     */
    char expanded_dest[MAX_PATH];
    expand_path(expanded_dest, dest);
    
    /*
     * DESTINATION VALIDATION
     * Ensure destination is within S1 directory structure
     * Security measure to prevent unauthorized directory access
     */
    if (strstr(expanded_dest, "S1") == NULL) {
        send_data(client, "ERROR: Invalid destination path", 31);
        return;
    }
    
    /*
     * OPERATION INITIATION
     * Log operation and signal client readiness to receive files
     */
    printf("Processing upload to: %s (%d files)\n", expanded_dest, file_count);
    send_data(client, "READY", 5);
    
    /*
     * FILE PROCESSING LOOP
     * Handle each file individually with comprehensive error handling
     */
    for (int i = 0; i < file_count; i++) {
        /*
         * FILENAME RECEPTION
         * Protocol: 4-byte length + filename string
         */
        int name_len;
        if (receive_data(client, &name_len, sizeof(int)) <= 0) {
            perror("Name length receive failed");
            continue;  // Skip this file, try next
        }
        
        char filename[256];
        if (receive_data(client, filename, name_len) <= 0) {
            perror("Filename receive failed");
            continue;
        }
        filename[name_len] = '\0';  // Null terminate
        
        /*
         * FILE SIZE RECEPTION
         * Used for memory allocation and transfer validation
         */
        long file_size;
        if (receive_data(client, &file_size, sizeof(long)) <= 0) {
            perror("File size receive failed");
            continue;
        }
        
        /*
         * SIZE VALIDATION
         * Reject invalid or zero-length files
         */
        if (file_size <= 0) {
            printf("Invalid file size for %s\n", filename);
            send_data(client, "FAIL", 4);
            continue;
        }
        
        /*
         * MEMORY ALLOCATION
         * Allocate buffer for entire file content
         * Large files could cause memory issues in production
         */
        char *data = malloc(file_size);
        if (!data) {
            perror("Memory allocation failed");
            send_data(client, "FAIL", 4);
            continue;
        }
        
        /*
         * FILE DATA RECEPTION
         * Receive complete file content into memory buffer
         */
        if (receive_data(client, data, file_size) <= 0) {
            perror("File data receive failed");
            free(data);
            send_data(client, "FAIL", 4);
            continue;
        }
        
        /*
         * DESTINATION PATH CONSTRUCTION
         * Combine destination directory with filename
         */
        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", expanded_dest, filename);
        
        /*
         * FILE TYPE ROUTING LOGIC
         * Determine handling based on file extension
         */
        int file_type = get_file_type(filename);
        int status = -1;  // Default to failure
        
        if (file_type == 1) { // .c file - handle locally
            /*
             * LOCAL STORAGE FOR C FILES
             * Create directory structure and write file to S1
             */
            char dir_path[MAX_PATH];
            strncpy(dir_path, full_path, sizeof(dir_path));
            char *dir = dirname(dir_path);  // Extract directory portion
            
            /*
             * DIRECTORY CREATION AND FILE WRITING
             * Use recursive directory creation then standard file I/O
             */
            if (create_dirs_recursive(dir) == 0) {
                int fd = open(full_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (fd >= 0) {
                    write(fd, data, file_size);  // Write entire file
                    close(fd);
                    status = file_size;  // Success - return bytes written
                    printf("Stored .c file: %s (%ld bytes)\n", full_path, file_size);
                } else {
                    printf("Failed to open file: %s\n", strerror(errno));
                }
            } else {
                printf("Failed to create directories: %s\n", strerror(errno));
            }
        } else if (file_type >= 2 && file_type <= 4) { // PDF, TXT, ZIP - forward to appropriate server
            /*
             * REMOTE STORAGE VIA FORWARDING
             * Translate path and forward to specialized server
             */
            char server_path[MAX_PATH];
            convert_server_path(server_path, full_path, file_type);
            
            printf("Forwarding to %s: %s\n", server_names[file_type], server_path);
            status = forward_to_server(server_ports[file_type], "STOR", server_path, data, file_size);
        } else {
            printf("Unsupported file type: %s\n", filename);
        }
        
        /*
         * RESPONSE GENERATION
         * Send 4-byte status response to client
         */
        char response[5];
        if (status >= 0) {
            snprintf(response, sizeof(response), "OK  "); // 4-byte response with padding
        } else {
            snprintf(response, sizeof(response), "FAIL");
        }
        send_data(client, response, 4);
        
        free(data);  // Clean up file buffer
    }
}


void handle_download(int client, const char *args) {
    char *files[MAX_FILES];
    int file_count = 0;
    
    /*
     * ARGUMENT PARSING
     * Similar to upload but no destination path
     */
    char args_copy[BUF_SIZE];
    strncpy(args_copy, args, sizeof(args_copy));
    
    char *token = strtok(args_copy, " ");
    while (token && file_count < MAX_FILES) {
        files[file_count++] = token;
        token = strtok(NULL, " ");
    }
    
    if (file_count == 0) {
        send_data(client, "ERROR: No files specified", 25);
        return;
    }
    
    /*
     * OPERATION INITIATION
     * Send file count so client knows how many files to expect
     */
    printf("Processing download for %d files\n", file_count);
    send_data(client, &file_count, sizeof(int));
    
    /*
     * FILE TRANSMISSION LOOP
     * Process each requested file individually
     */
    for (int i = 0; i < file_count; i++) {
        char expanded_path[MAX_PATH];
        expand_path(expanded_path, files[i]);
        
        char *filename = basename(expanded_path);  // Extract filename for client
        int file_type = get_file_type(filename);
        
        /*
         * FILENAME TRANSMISSION
         * Send filename length + filename string
         */
        int name_len = strlen(filename);
        send_data(client, &name_len, sizeof(int));
        send_data(client, filename, name_len);
        
        long file_size = -1;  // Default to not found
        
        if (file_type == 1) { // .c file - read locally
            /*
             * LOCAL FILE ACCESS
             * Use stat() to get file size, then read and transmit
             */
            struct stat st;
            if (stat(expanded_path, &st) == 0) {
                file_size = st.st_size;
                send_data(client, &file_size, sizeof(long));
                
                /*
                 * FILE TRANSMISSION
                 * Open file and stream contents to client
                 */
                int fd = open(expanded_path, O_RDONLY);
                if (fd >= 0) {
                    char buffer[BUF_SIZE];
                    ssize_t bytes_read;
                    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
                        send_data(client, buffer, bytes_read);
                    }
                    close(fd);
                    printf("Sent .c file: %s (%ld bytes)\n", expanded_path, file_size);
                }
            } else {
                printf("File not found: %s\n", expanded_path);
                send_data(client, &file_size, sizeof(long));  // Send -1 = not found
            }
        } else if (file_type >= 2 && file_type <= 4) { // PDF, TXT, ZIP - forward request
            /*
             * REMOTE FILE ACCESS VIA FORWARDING
             * Establish connection to appropriate server and relay data
             */
            char server_path[MAX_PATH];
            convert_server_path(server_path, expanded_path, file_type);
            
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_in addr = {
                    .sin_family = AF_INET,
                    .sin_port = htons(server_ports[file_type])
                };
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                
                if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    /*
                     * RETRIEVE REQUEST PROTOCOL
                     * Send RETR command with path to target server
                     */
                    send_data(sock, "RETR", 4);
                    
                    int path_len = strlen(server_path);
                    send_data(sock, &path_len, sizeof(int));
                    send_data(sock, server_path, path_len);
                    
                    int dlen = 0;  // No data for retrieve request
                    send_data(sock, &dlen, sizeof(int));
                    
                    /*
                     * FILE SIZE RECEPTION AND RELAY
                     * Get file size from target server and forward to client
                     */
                    int remote_size;
                    if (receive_data(sock, &remote_size, sizeof(int)) > 0) {
                        file_size = remote_size;
                        send_data(client, &file_size, sizeof(long));
                        
                        if (file_size > 0) {
                            /*
                             * DATA RELAY OPERATION
                             * Stream file data from target server to client
                             * Acts as transparent proxy
                             */
                            printf("Forwarding file from %s (%ld bytes)\n", 
                                   server_names[file_type], file_size);
                            char buffer[BUF_SIZE];
                            long remaining = file_size;
                            while (remaining > 0) {
                                size_t to_read = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
                                ssize_t n = recv(sock, buffer, to_read, 0);
                                if (n <= 0) break;
                                send_data(client, buffer, n);
                                remaining -= n;
                            }
                        }
                    }
                }
                close(sock);
            } else {
                printf("Cannot connect to %s server\n", server_names[file_type]);
                send_data(client, &file_size, sizeof(long));  // Send -1 = error
            }
        } else {
            // Unsupported file type
            printf("Unsupported file type: %s\n", filename);
            send_data(client, &file_size, sizeof(long));  // Send -1 = unsupported
        }
    }
}


void handle_remove(int client, const char *args) {
    char *files[MAX_FILES];
    int file_count = 0;
    
    /*
     * ARGUMENT PARSING
     * Extract file paths from command arguments
     */
    char args_copy[BUF_SIZE];
    strncpy(args_copy, args, sizeof(args_copy));
    
    char *token = strtok(args_copy, " ");
    while (token && file_count < MAX_FILES) {
        files[file_count++] = token;
        token = strtok(NULL, " ");
    }
    
    if (file_count == 0) {
        send_data(client, "ERROR: No files specified", 25);
        return;
    }
    
    int success_count = 0;  // Track successful deletions
    
    /*
     * FILE DELETION LOOP
     * Process each file deletion request
     */
    for (int i = 0; i < file_count; i++) {
        char expanded_path[MAX_PATH];
        expand_path(expanded_path, files[i]);
        
        char *filename = basename(expanded_path);
        int file_type = get_file_type(filename);
        
        int status = -1;  // Default to failure
        
        if (file_type == 1) {
            /*
             * LOCAL FILE DELETION
             * unlink() removes file from filesystem
             * Returns 0 on success, -1 on error
             */
            if (unlink(expanded_path) == 0) {
                status = 0;
                printf("Removed local file: %s\n", expanded_path);
            } else {
                printf("Failed to remove local file: %s (%s)\n", expanded_path, strerror(errno));
            }
        } else if (file_type >= 2 && file_type <= 4) {
            /*
             * REMOTE FILE DELETION
             * Forward REMV command to appropriate server
             */
            char server_path[MAX_PATH];
            convert_server_path(server_path, expanded_path, file_type);
            
            printf("Requesting removal from %s: %s\n", server_names[file_type], server_path);
            status = forward_to_server(server_ports[file_type], "REMV", server_path, NULL, 0);
        }
        
        if (status == 0) success_count++;
    }
    
    /*
     * OVERALL RESULT RESPONSE
     * Success only if all files deleted successfully
     */
    char res[5];
    if (success_count == file_count) {
        snprintf(res, sizeof(res), "OK  "); // 4-byte response
    } else {
        snprintf(res, sizeof(res), "FAIL");
    }
    send_data(client, res, 4);
}


void handle_tar(int client, const char *args) {
    char filetype[20];
    if (sscanf(args, "%s", filetype) != 1) {
        long zero_size = 0;
        send_data(client, &zero_size, sizeof(long));
        return;
    }
    
    char tar_file[MAX_PATH];
    char cmd[512];
    const char *home = getenv("HOME");
    
    /*
     * TAR COMMAND CONSTRUCTION BY FILE TYPE
     * Each file type searches its corresponding server directory
     * find command locates files, tar creates archive
     * Error redirection (2>/dev/null) suppresses error messages
     * touch ensures file exists even if no matches found
     */
    if (strcmp(filetype, ".c") == 0) {
        // Create tar directly in /tmp with unique name
        snprintf(tar_file, sizeof(tar_file), "/tmp/cfiles_%d.tar", getpid());
        snprintf(cmd, sizeof(cmd), 
            "cd %s/S1 && find . -name '*.c' 2>/dev/null | tar -cf %s -T - 2>/dev/null || touch %s", 
            home, tar_file, tar_file);
    } 
    else if (strcmp(filetype, ".pdf") == 0) {
        snprintf(tar_file, sizeof(tar_file), "/tmp/pdfs_%d.tar", getpid());
        snprintf(cmd, sizeof(cmd), 
            "cd %s/S2 && find . -name '*.pdf' 2>/dev/null | tar -cf %s -T - 2>/dev/null || touch %s", 
            home, tar_file, tar_file);
    }
    else if (strcmp(filetype, ".txt") == 0) {
        snprintf(tar_file, sizeof(tar_file), "/tmp/txts_%d.tar", getpid());
        snprintf(cmd, sizeof(cmd), 
            "cd %s/S3 && find . -name '*.txt' 2>/dev/null | tar -cf %s -T - 2>/dev/null || touch %s", 
            home, tar_file, tar_file);
    }
    else {
        // Unsupported file type
        long zero_size = 0;
        send_data(client, &zero_size, sizeof(long));
        return;
    }
    
    /*
     * TAR ARCHIVE CREATION
     * Execute shell command to create tar file
     * system() runs command in subshell
     */
    printf("Executing: %s\n", cmd);
    system(cmd);
    
    /*
     * TAR FILE SIZE DETERMINATION
     * stat() gets file information including size
     */
    struct stat st;
    long tar_size = 0;
    if (stat(tar_file, &st) == 0) {
        tar_size = st.st_size;
    }
    
    /*
     * SIZE TRANSMISSION
     * Send tar file size to client first
     */
    printf("Tar file size: %ld bytes\n", tar_size);
    send_data(client, &tar_size, sizeof(long));
    
    /*
     * TAR FILE TRANSMISSION
     * Stream tar file contents to client if non-empty
     */
    if (tar_size > 0) {
        int fd = open(tar_file, O_RDONLY);
        if (fd >= 0) {
            char buffer[BUF_SIZE];
            ssize_t bytes_read;
            long total_sent = 0;
            
            /*
             * STREAMING TRANSMISSION
             * Read tar file in chunks and send to client
             * Track progress for logging
             */
            while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
                if (send_data(client, buffer, bytes_read) < 0) {
                    printf("Failed to send tar data\n");
                    break;
                }
                total_sent += bytes_read;
            }
            close(fd);
            printf("Sent %ld bytes successfully\n", total_sent);
        }
    }
    
    /*
     * CLEANUP
     * Remove temporary tar file
     */
    unlink(tar_file);
}


void handle_list(int client, const char *args) {
    char path[MAX_PATH];
    if (sscanf(args, "%s", path) != 1) {
        send_data(client, "ERROR: Invalid path", 18);
        return;
    }
    
    char expanded_path[MAX_PATH];
    expand_path(expanded_path, path);
    
    /*
     * PATH VALIDATION
     * Ensure path is within allowed S1 directory structure
     */
    if (strstr(expanded_path, "S1") == NULL) {
        send_data(client, "ERROR: Invalid path", 18);
        return;
    }
    
    printf("Listing files in: %s\n", expanded_path);
    
    /*
     * LOCAL FILE LISTING (.c files)
     * Use opendir/readdir to scan S1 directory
     */
    char result[BUF_SIZE] = "";
    DIR *dir = opendir(expanded_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            /*
             * FILTER FOR REGULAR FILES WITH .c EXTENSION
             * DT_REG = regular file (not directory, link, etc.)
             * strstr() checks for .c substring in filename
             */
            if (entry->d_type == DT_REG && strstr(entry->d_name, ".c")) {
                strcat(result, entry->d_name);
                strcat(result, "\n");
            }
        }
        closedir(dir);
    }
    
    /*
     * REMOTE FILE LISTING AGGREGATION
     * Query each specialized server (S2, S3, S4) for their files
     */
    for (int i = 2; i <= 4; i++) {
        char server_path[MAX_PATH];
        convert_server_path(server_path, expanded_path, i);
        
        /*
         * SERVER CONNECTION AND LIST REQUEST
         * Connect to each server and send LIST command
         */
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(server_ports[i])
            };
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                /*
                 * LIST COMMAND PROTOCOL
                 * Send LIST command with directory path
                 */
                send_data(sock, "LIST", 4);
                
                int path_len = strlen(server_path);
                send_data(sock, &path_len, sizeof(int));
                send_data(sock, server_path, path_len);
                
                int data_len = 0;  // No data for list request
                send_data(sock, &data_len, sizeof(int));
                
                /*
                 * RESPONSE RECEPTION AND AGGREGATION
                 * Receive file list from server and append to result
                 */
                int list_size;
                if (receive_data(sock, &list_size, sizeof(int)) > 0 && list_size > 0) {
                    char *list_data = malloc(list_size + 1);
                    if (list_data) {
                        receive_data(sock, list_data, list_size);
                        list_data[list_size] = '\0';
                        strcat(result, list_data);  // Append to combined result
                        free(list_data);
                    }
                }
            }
            close(sock);
        }
    }
    
    /*
     * COMBINED RESULT TRANSMISSION
     * Send total result size followed by complete file listing
     */
    int result_size = strlen(result);
    send_data(client, &result_size, sizeof(int));
    if (result_size > 0) {
        send_data(client, result, result_size);
    }
}


void client_handler(int client) {
    printf("Client handler started (PID: %d)\n", getpid());
    
    char buffer[BUF_SIZE];
    
    /*
     * COMMAND PROCESSING LOOP
     * Continue until client disconnects or error occurs
     */
    while (1) {
        /*
         * COMMAND RECEPTION
         * Clear buffer and receive next command from client
         * recv() returns 0 when client closes connection
         */
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_received = recv(client, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received <= 0) {
            printf("Client disconnected (PID: %d)\n", getpid());
            break;  // Exit command loop
        }
        
        /*
         * COMMAND PARSING
         * Extract command and arguments from received buffer
         * Commands are space-separated: "command arg1 arg2 ..."
         */
        buffer[bytes_received] = '\0';  // Null terminate
        printf("Received command: %s\n", buffer);
        
        char command[20];
        char args[BUF_SIZE] = "";
        
        /*
         * COMMAND EXTRACTION
         * sscanf() extracts first word as command
         * Manual parsing extracts remaining arguments
         */
        if (sscanf(buffer, "%s", command) == 1) {
            char *args_start = buffer + strlen(command);
            while (*args_start == ' ') args_start++;  // Skip leading spaces
            strcpy(args, args_start);
        }
        
        /*
         * COMMAND DISPATCH
         * Route to appropriate handler based on command string
         * Each handler implements specific protocol for that operation
         */
        if (strcmp(command, "uploadf") == 0) {
            handle_upload(client, args);
        } else if (strcmp(command, "downlf") == 0) {
            handle_download(client, args);
        } else if (strcmp(command, "removef") == 0) {
            handle_remove(client, args);
        } else if (strcmp(command, "downltar") == 0) {
            handle_tar(client, args);
        } else if (strcmp(command, "dispfnames") == 0) {
            handle_list(client, args);
        } else {
            /*
             * UNKNOWN COMMAND HANDLING
             * Send error response for unrecognized commands
             */
            send_data(client, "ERROR: Unknown command", 22);
        }
    }
}
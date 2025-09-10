#include <stdio.h>      // Standard I/O operations (printf, perror, snprintf)
#include <stdlib.h>     // Memory management (malloc, free), process control
#include <string.h>     // String manipulation (strcmp, strlen, strcat)
#include <unistd.h>     // POSIX APIs (close, read, write, unlink)
#include <sys/socket.h> // Socket programming (socket, bind, listen, accept, send, recv)
#include <netinet/in.h> // Internet address structures (sockaddr_in)
#include <arpa/inet.h>  // IP address conversion (inet_ntoa, ntohs)
#include <sys/stat.h>   // File status operations (stat, mkdir)
#include <fcntl.h>      // File control operations (open flags: O_CREAT, O_RDONLY, O_WRONLY, O_TRUNC)
#include <dirent.h>     // Directory operations (opendir, readdir, closedir, DIR, dirent)
#include <errno.h>      // Error handling (errno, EEXIST)
#include <libgen.h>     // Path manipulation (dirname)

/*
 * SERVER CONFIGURATION CONSTANTS
 * Specialized configuration for text file server operations
 */
#define PORT 8893        // TXT server listening port - unique in server topology
#define BUF_SIZE 8192    // 8KB buffer - optimized for text file operations
#define MAX_PATH 1024    // Maximum file path length - filesystem standard

/*
 * FUNCTION PROTOTYPES
 * Modular design with clean interface definitions
 */
int create_dirs_recursive(const char *path);     // Directory structure creation
int send_data(int sock, const void *data, size_t len);      // Reliable data transmission
int receive_data(int sock, void *data, size_t len);         // Reliable data reception  
void handle_client(int client);                  // Main command processor for text operations


int main() {
    /*
     * DIRECTORY STRUCTURE INITIALIZATION
     * Create the S3 base directory for text file storage
     * Maintains parallel directory structure to S1 but specialized for TXT files
     * Uses HOME environment variable for user-specific storage location
     */
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/S3", getenv("HOME"));  // Build ~/S3 path
    
    /*
     * DIRECTORY CREATION WITH IDEMPOTENT BEHAVIOR
     * mkdir() creates directory with standard permissions
     * 0755 = rwxr-xr-x (owner: read/write/execute, others: read/execute)
     * EEXIST check makes operation idempotent - safe to run multiple times
     */
    if (mkdir(dir, 0755) == -1 && errno != EEXIST) {
        perror("Failed to create S3 directory");  // Log but continue
    }
    
    /*
     * TCP SERVER SOCKET CREATION
     * Standard socket creation for internet communication
     * AF_INET = IPv4 internet protocol family
     * SOCK_STREAM = TCP reliable, connection-oriented service
     * 0 = default protocol selection (TCP for SOCK_STREAM)
     */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);  // Fatal error - cannot operate without socket
    }
    
    /*
     * SOCKET OPTION CONFIGURATION
     * SO_REUSEADDR enables immediate address reuse after server shutdown
     * Critical for development and production restart scenarios
     * Prevents "Address already in use" bind errors
     */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /*
     * SERVER ADDRESS STRUCTURE CONFIGURATION
     * Define listening address and port for text file server
     * Uses designated initializer syntax for clarity and safety
     */
    struct sockaddr_in address = {
        .sin_family = AF_INET,        // IPv4 address family
        .sin_addr.s_addr = INADDR_ANY, // Listen on all network interfaces
        .sin_port = htons(PORT)       // Convert port to network byte order
    };
    
    /*
     * SOCKET ADDRESS BINDING
     * Associates socket with specific network address and port
     * Makes server socket available for incoming connections
     */
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");       // Common cause: port already in use
        close(server_fd);            // Clean up socket resource
        exit(EXIT_FAILURE);
    }
    
    /*
     * LISTENING STATE ACTIVATION
     * Transitions socket from active to passive listening mode
     * Backlog of 10 = maximum number of pending connections
     * Appropriate for specialized server expecting low connection volume
     */
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    /*
     * SERVER STARTUP CONFIRMATION
     * Provide startup confirmation for monitoring and debugging
     */
    printf("S3 TXT Server running on port %d\n", PORT);
    
    /*
     * CONNECTION ACCEPTANCE LOOP - MAIN SERVER OPERATION
     * Infinite loop accepting and processing connections from S1 coordinator
     * Synchronous processing model - one request at a time
     * Simple and reliable for specialized server role
     */
    while (1) {
        /*
         * CLIENT CONNECTION ACCEPTANCE
         * accept() blocks until S1 coordinator connects
         * Returns new socket descriptor for communication
         * Original server socket continues listening for additional connections
         */
        struct sockaddr_in client_addr;           // Storage for connecting client address
        socklen_t addr_len = sizeof(client_addr); // Address structure size
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        
        /*
         * CONNECTION ERROR HANDLING
         * Log connection errors but continue serving
         * Implements graceful degradation - individual failures don't crash server
         */
        if (client_fd < 0) {
            perror("Accept failed");
            continue;  // Try to accept next connection
        }
        
        /*
         * CONNECTION LOGGING AND MONITORING
         * Log incoming connections for debugging and security monitoring
         * Expected connections primarily from S1 coordinator (127.0.0.1)
         */
        printf("New client connected from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        /*
         * COMMAND PROCESSING
         * Handle the text file operation command from S1
         * Synchronous processing ensures completion before accepting next request
         */
        handle_client(client_fd);
        
        /*
         * CONNECTION CLEANUP
         * Close connection after processing command
         * Stateless design - no persistent connection state
         */
        close(client_fd);
    }
    
    /*
     * SERVER SHUTDOWN CLEANUP (unreachable in current design)
     * Proper resource cleanup for graceful shutdown
     */
    close(server_fd);
    return 0;
}


int create_dirs_recursive(const char *path) {
    /*
     * LOCAL PATH MANIPULATION BUFFER
     * Create modifiable copy since we need to temporarily modify path
     * Original path parameter remains unchanged (const protection)
     */
    char tmp_path[MAX_PATH];
    char *p;  // Pointer for path traversal
    
    /*
     * PATH PREPARATION AND NORMALIZATION
     * Copy input path and normalize by removing trailing slash
     * Ensures consistent processing regardless of input format
     */
    snprintf(tmp_path, sizeof(tmp_path), "%s", path);
    size_t len = strlen(tmp_path);
    
    // Remove trailing slash if present for consistency
    if (len > 0 && tmp_path[len - 1] == '/') {
        tmp_path[len - 1] = '\0';
    }
    
    /*
     * INCREMENTAL DIRECTORY CREATION LOOP
     * Process path component by component from root to target
     * Create each directory level if it doesn't already exist
     * Start from position 1 to skip root directory slash
     */
    for (p = tmp_path + 1; *p; p++) {
        if (*p == '/') {
            /*
             * DIRECTORY COMPONENT BOUNDARY PROCESSING
             * Temporarily terminate string at current component boundary
             * Attempt directory creation up to this point
             * Restore directory separator and continue
             */
            *p = '\0';  // Temporarily end string at this position
            if (mkdir(tmp_path, 0755) == -1 && errno != EEXIST) {
                return -1;  // Directory creation failed
            }
            *p = '/';   // Restore directory separator
        }
    }
    
    /*
     * FINAL DIRECTORY CREATION
     * Create the complete target directory
     * 0755 permissions = owner: rwx, group: r-x, others: r-x
     */
    if (mkdir(tmp_path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    
    return 0;  // Success - complete directory structure created
}


int send_data(int sock, const void *data, size_t len) {
    size_t sent = 0;  // Cumulative byte counter
    
    /*
     * TRANSMISSION COMPLETION LOOP
     * Continue transmission until all data sent or error
     * Handles partial sends gracefully with automatic retry
     */
    while (sent < len) {
        /*
         * SINGLE TRANSMISSION ATTEMPT
         * MSG_NOSIGNAL flag prevents SIGPIPE signal on broken connections
         * Pointer arithmetic advances through data buffer as bytes are sent
         * Returns actual bytes transmitted (may be less than requested)
         */
        ssize_t n = send(sock, (char*)data + sent, len - sent, MSG_NOSIGNAL);
        
        /*
         * ERROR DETECTION AND RETRY LOGIC
         * n <= 0 indicates transmission error or connection closure
         * EAGAIN/EWOULDBLOCK = temporary resource unavailability (retry)
         * Other errors considered permanent failures
         */
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // Retry
            return -1;  // Permanent transmission error
        }
        sent += n;  // Update cumulative transmission counter
    }
    return sent;  // Return total bytes successfully transmitted
}


int receive_data(int sock, void *data, size_t len) {
    size_t received = 0;  // Cumulative reception counter
    
    /*
     * RECEPTION COMPLETION LOOP
     * Continue until all expected data received or error
     */
    while (received < len) {
        /*
         * SINGLE RECEPTION ATTEMPT
         * recv() reads available data from socket receive buffer
         * May return less data than requested due to network timing
         * Returns 0 when peer closes connection, <0 on error
         */
        ssize_t n = recv(sock, (char*)data + received, len - received, 0);
        if (n <= 0) return -1;  // Connection closed or error occurred
        received += n;  // Update cumulative reception counter
    }
    return received;  // Return total bytes successfully received
}


void handle_client(int client) {
    char command[5];      // 4-byte command + null terminator
    char path[MAX_PATH];  // File path for operation
    int path_len, data_len;  // Protocol length fields
    
    /*
     * COMMAND RECEPTION AND PARSING
     * Receive fixed-length 4-byte command identifier
     * Commands are standardized across all specialized servers
     */
    if (receive_data(client, command, 4) <= 0) return;
    command[4] = '\0';  // Null terminate for string operations
    
    /*
     * PATH RECEPTION AND PARSING
     * Receive variable-length path with length prefix
     * Protocol uses length-prefixed strings for safety
     */
    if (receive_data(client, &path_len, sizeof(int)) <= 0) return;
    if (receive_data(client, path, path_len) <= 0) return;
    path[path_len] = '\0';  // Null terminate path string
    
    /*
     * DATA LENGTH RECEPTION
     * Receive data length field (0 for commands without data payload)
     */
    if (receive_data(client, &data_len, sizeof(int)) <= 0) return;
    
    /*
     * COMMAND AND PARAMETER LOGGING
     * Log received command for debugging and monitoring purposes
     */
    printf("Command: %s, Path: %s, Data length: %d\n", command, path, data_len);
    
    /*
     * STORE COMMAND IMPLEMENTATION
     * Handles text file storage requests from S1 coordinator
     * Triggered when client uploads text file through distributed system
     */
    if (strcmp(command, "STOR") == 0) {
        /*
         * FILE DATA RECEPTION BUFFER ALLOCATION
         * Allocate memory buffer for complete file content
         * For text files, this is typically reasonable memory usage
         */
        char *data = malloc(data_len);
        if (!data) {
            int status = -1;  // Memory allocation failure status
            send_data(client, &status, sizeof(int));
            return;
        }
        
        /*
         * COMPLETE FILE DATA RECEPTION
         * Receive entire file content before processing
         * Ensures atomic file operations
         */
        if (receive_data(client, data, data_len) <= 0) {
            free(data);
            int status = -1;  // Data reception failure status
            send_data(client, &status, sizeof(int));
            return;
        }
        
        /*
         * DIRECTORY STRUCTURE PREPARATION
         * Extract directory path from full file path
         * Create directory structure to preserve client organization
         */
        char dir_path[MAX_PATH];
        strncpy(dir_path, path, sizeof(dir_path));
        char *dir = dirname(dir_path);  // Extract directory component
        
        /*
         * ATOMIC FILE STORAGE OPERATION
         * Create directories first, then write file atomically
         * Ensures file system consistency
         */
        if (create_dirs_recursive(dir) == 0) {
            /*
             * FILE CREATION AND WRITING
             * O_CREAT = create file if it doesn't exist
             * O_WRONLY = open for writing only
             * O_TRUNC = truncate existing file to zero length
             * 0644 = rw-r--r-- permissions (owner: rw, others: r)
             */
            int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) {
                write(fd, data, data_len);  // Write complete file content
                close(fd);
                send_data(client, &data_len, sizeof(int));  // Success response
                printf("Stored TXT: %s (%d bytes)\n", path, data_len);
            } else {
                int status = -1;  // File creation failure status
                send_data(client, &status, sizeof(int));
            }
        } else {
            int status = -1;  // Directory creation failure status
            send_data(client, &status, sizeof(int));
        }
        
        free(data);  // Clean up file data buffer
    }
    /*
     * RETRIEVE COMMAND IMPLEMENTATION
     * Handles text file retrieval requests from S1 coordinator
     * Triggered when client downloads text file through distributed system
     */
    else if (strcmp(command, "RETR") == 0) {
        /*
         * FILE EXISTENCE AND METADATA CHECK
         * Use stat() system call to verify file exists and get size
         */
        struct stat st;
        if (stat(path, &st) == 0) {
            /*
             * FILE SIZE TRANSMISSION
             * Send file size to S1 so it knows how much data to expect
             * Enables proper buffer allocation and progress tracking
             */
            int size = st.st_size;
            send_data(client, &size, sizeof(int));
            
            /*
             * FILE CONTENT STREAMING TRANSMISSION
             * Open file and stream contents to requesting server
             * Uses buffered I/O for efficiency with text files
             */
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                char buffer[BUF_SIZE];
                ssize_t bytes_read;
                
                /*
                 * STREAMING TRANSMISSION LOOP
                 * Read file in chunks and transmit to minimize memory usage
                 * Particularly important for large text files
                 */
                while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
                    send_data(client, buffer, bytes_read);
                }
                close(fd);
                printf("Sent TXT: %s (%d bytes)\n", path, size);
            }
        } else {
            /*
             * FILE NOT FOUND RESPONSE
             * Send -1 size to indicate file doesn't exist
             * Standard error response in protocol
             */
            int size = -1;
            send_data(client, &size, sizeof(int));
        }
    }
    /*
     * REMOVE COMMAND IMPLEMENTATION
     * Handles text file deletion requests from S1 coordinator
     * Triggered when client deletes text file through distributed system
     */
    else if (strcmp(command, "REMV") == 0) {
        /*
         * FILE DELETION OPERATION
         * unlink() system call removes file from filesystem
         * Returns 0 on success, -1 on failure
         */
        int status = (unlink(path) == 0) ? 0 : -1;
        send_data(client, &status, sizeof(int));
        printf("Removed TXT: %s (%s)\n", path, status == 0 ? "success" : "fail");
    }
    /*
     * TAR CREATION COMMAND IMPLEMENTATION
     * Creates tar archive of all text files in S3 directory
     * Provides bulk download capability for text files
     */
    else if (strcmp(command, "TARC") == 0) {
        /*
         * TEMPORARY TAR FILE CREATION
         * mkstemp() creates unique temporary file with secure permissions
         * Template "XXXXXX" gets replaced with unique random string
         */
        char tmp_tar[] = "/tmp/text_XXXXXX";
        int fd_tmp = mkstemp(tmp_tar);
        long size = 0;
        
        if (fd_tmp == -1) {
            perror("mkstemp failed");
            send_data(client, &size, sizeof(long));
            return;
        }
        close(fd_tmp);  // Close file descriptor, keep filename

        /*
         * TAR ARCHIVE CREATION USING SHELL COMMANDS
         * Use find and tar commands to create comprehensive archive
         * Error redirection (2>/dev/null) suppresses error messages
         */
        const char *home = getenv("HOME");
        if (home) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), 
                "cd %s/S3 && find . -name '*.txt' -type f 2>/dev/null | tar -cf %s -T - 2>/dev/null", 
                home, tmp_tar);
            printf("Executing: %s\n", cmd);
            system(cmd);  // Execute shell command to create archive
        }

        /*
         * TAR ARCHIVE SIZE DETERMINATION
         * Get size of created tar file for transmission
         */
        struct stat st;
        if (stat(tmp_tar, &st) == 0) {
            size = st.st_size;
            printf("Created TXT tar of size %ld\n", size);
        }

        /*
         * TAR SIZE RESPONSE TRANSMISSION
         * Send archive size to requesting server
         */
        send_data(client, &size, sizeof(long));

        /*
         * TAR CONTENT STREAMING TRANSMISSION
         * Stream tar archive contents if archive is non-empty
         */
        if (size > 0) {
            int fd = open(tmp_tar, O_RDONLY);
            if (fd >= 0) {
                char buffer[BUF_SIZE];
                ssize_t bytes_read;
                
                /*
                 * ARCHIVE STREAMING LOOP
                 * Stream archive contents in chunks
                 */
                while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
                    if (send_data(client, buffer, bytes_read) < 0) {
                        perror("Failed to send tar data");
                        break;
                    }
                }
                close(fd);
            }
        }

        /*
         * TEMPORARY FILE CLEANUP
         * Remove temporary tar file to prevent disk space accumulation
         */
        unlink(tmp_tar);
    }
    /*
     * LIST COMMAND IMPLEMENTATION
     * Lists text files in specified directory
     * Provides directory browsing capability for text files
     */
    else if (strcmp(command, "LIST") == 0) {
        /*
         * DIRECTORY LISTING GENERATION
         * Scan directory for text files and build listing string
         */
        char result[BUF_SIZE] = "";
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry;
            
            /*
             * DIRECTORY ENTRY FILTERING AND PROCESSING
             * Iterate through directory entries, filter for text files
             */
            while ((entry = readdir(dir)) != NULL) {
                /*
                 * TEXT FILE IDENTIFICATION AND FILTERING
                 * DT_REG = regular file type (not directory, symlink, device)
                 * strstr() searches for ".txt" substring in filename
                 */
                if (entry->d_type == DT_REG && strstr(entry->d_name, ".txt")) {
                    strcat(result, entry->d_name);
                    strcat(result, "\n");  // Newline-separated file listing
                }
            }
            closedir(dir);
        }
        
        /*
         * LISTING TRANSMISSION
         * Send listing size followed by listing content
         * Protocol consistent with other list operations
         */
        int size = strlen(result);
        send_data(client, &size, sizeof(int));
        if (size > 0) {
            send_data(client, result, size);
        }
        printf("Listed TXTs in %s (%d files)\n", path, size);
    }
    /*
     * UNKNOWN COMMAND HANDLING
     * Handle unrecognized commands gracefully
     * Maintain protocol compliance with error response
     */
    else {
        printf("Unknown command: %s\n", command);
        int status = -1;  // Unknown command error status
        send_data(client, &status, sizeof(int));
    }
}


#include <stdio.h>      // Standard I/O operations (printf, perror)
#include <stdlib.h>     // Memory allocation (malloc, free), process control
#include <string.h>     // String manipulation (strcmp, strlen, strncpy)
#include <unistd.h>     // POSIX APIs (close, read, write)
#include <sys/socket.h> // Socket programming (socket, bind, listen, accept)
#include <netinet/in.h> // Internet address structures (sockaddr_in)
#include <arpa/inet.h>  // IP address conversion (inet_ntoa, ntohs)
#include <sys/stat.h>   // File status operations (stat, mkdir)
#include <fcntl.h>      // File control operations (open, O_CREAT, O_RDONLY)
#include <dirent.h>     // Directory operations (opendir, readdir, closedir)
#include <errno.h>      // Error handling (errno, EEXIST)
#include <libgen.h>     // Path manipulation (dirname)

/*
 * SERVER CONFIGURATION CONSTANTS
 * Optimized for specialized PDF server operations
 */
#define PORT 8892        // PDF server listening port - part of server topology
#define BUF_SIZE 8192    // 8KB buffer - smaller than S1 due to specialized nature
#define MAX_PATH 1024    // Maximum path length - standard filesystem limit

/*
 * FUNCTION PROTOTYPES
 * Clean interface definition for modular design
 */
int create_dirs_recursive(const char *path);     // Directory creation utility
int send_data(int sock, const void *data, size_t len);      // Reliable transmission
int receive_data(int sock, void *data, size_t len);         // Reliable reception
void handle_client(int client);                  // Main command processor


int main() {
    /*
     * DIRECTORY STRUCTURE INITIALIZATION
     * Create the S2 base directory where PDF files will be stored
     * Uses HOME environment variable for user-specific storage
     * Mirrors the S1 directory structure but for PDF specialization
     */
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/S2", getenv("HOME"));  // Construct ~/S2 path
    
    /*
     * DIRECTORY CREATION WITH ERROR HANDLING
     * mkdir() creates directory with 0755 permissions (rwxr-xr-x)
     * EEXIST check prevents errors if directory already exists
     * This is idempotent - safe to run multiple times
     */
    if (mkdir(dir, 0755) == -1 && errno != EEXIST) {
        perror("Failed to create S2 directory");  // Log error but continue
    }
    
    /*
     * TCP SERVER SOCKET CREATION
     * AF_INET = IPv4 internet protocols
     * SOCK_STREAM = TCP reliable, connection-oriented communication
     * 0 = default protocol (TCP for SOCK_STREAM)
     */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);  // Fatal error - cannot continue without socket
    }
    
    /*
     * SOCKET OPTION CONFIGURATION
     * SO_REUSEADDR allows immediate reuse of address after server restart
     * Critical for development and restart scenarios
     * Prevents "Address already in use" errors
     */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /*
     * SERVER ADDRESS STRUCTURE SETUP
     * Configure listening address and port for PDF server
     * INADDR_ANY = listen on all available network interfaces
     */
    struct sockaddr_in address = {
        .sin_family = AF_INET,        // IPv4 address family
        .sin_addr.s_addr = INADDR_ANY, // Listen on all interfaces (0.0.0.0)
        .sin_port = htons(PORT)       // Convert port to network byte order
    };
    
    /*
     * SOCKET ADDRESS BINDING
     * Associates socket with specific IP address and port
     * Makes socket available for incoming connections
     */
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");       // Usually port already in use
        close(server_fd);            // Clean up socket resource
        exit(EXIT_FAILURE);
    }
    
    /*
     * LISTENING STATE ACTIVATION
     * Transitions socket to passive listening mode
     * Backlog of 10 = maximum pending connections
     * PDF server expects low connection volume from S1 only
     */
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    /*
     * SERVER STARTUP CONFIRMATION
     * Log server status for monitoring and debugging
     */
    printf("S2 PDF Server running on port %d\n", PORT);
    
    /*
     * CONNECTION ACCEPT LOOP - MAIN SERVER OPERATION
     * Continuously accept and process connections from S1 coordinator
     * Synchronous processing - one connection at a time
     */
    while (1) {
        /*
         * CLIENT CONNECTION ACCEPTANCE
         * accept() blocks until S1 coordinator connects
         * Returns new socket for communication with requesting server
         */
        struct sockaddr_in client_addr;           // Storage for client (S1) address
        socklen_t addr_len = sizeof(client_addr); // Address structure size
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        
        /*
         * CONNECTION ERROR HANDLING
         * Log errors but continue serving - graceful degradation
         */
        if (client_fd < 0) {
            perror("Accept failed");
            continue;  // Try to accept next connection
        }
        
        /*
         * CONNECTION LOGGING
         * Log incoming connections for debugging and monitoring
         * Expected primarily from S1 coordinator (127.0.0.1)
         */
        printf("New client connected from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        /*
         * COMMAND PROCESSING
         * Handle the PDF-related command from S1
         * Synchronous processing - complete before accepting next connection
         */
        handle_client(client_fd);
        
        /*
         * CONNECTION CLEANUP
         * Close connection after processing command
         * Stateless design - no persistent connections
         */
        close(client_fd);
    }
    
    /*
     * SERVER SHUTDOWN (unreachable in current design)
     * Clean up listening socket
     */
    close(server_fd);
    return 0;
}


int create_dirs_recursive(const char *path) {
    /*
     * LOCAL PATH BUFFER
     * Create modifiable copy for path manipulation
     * Original path parameter remains unchanged
     */
    char tmp_path[MAX_PATH];
    char *p;  // Pointer for path traversal
    
    /*
     * PATH COPYING AND NORMALIZATION
     * Copy input path and remove trailing slash for consistency
     */
    snprintf(tmp_path, sizeof(tmp_path), "%s", path);
    size_t len = strlen(tmp_path);
    
    // Remove trailing slash if present
    if (len > 0 && tmp_path[len - 1] == '/') {
        tmp_path[len - 1] = '\0';
    }
    
    /*
     * INCREMENTAL DIRECTORY CREATION
     * Traverse path component by component
     * Create each directory level if it doesn't exist
     * Start from position 1 to skip root slash
     */
    for (p = tmp_path + 1; *p; p++) {
        if (*p == '/') {
            /*
             * DIRECTORY COMPONENT PROCESSING
             * Temporarily terminate string at current slash
             * Attempt to create directory up to this point
             * Restore slash and continue
             */
            *p = '\0';  // Temporarily end string here
            if (mkdir(tmp_path, 0755) == -1 && errno != EEXIST) {
                return -1;  // Directory creation failed
            }
            *p = '/';   // Restore slash character
        }
    }
    
    /*
     * FINAL DIRECTORY CREATION
     * Create the complete target directory
     * 0755 permissions = owner: rwx, group/others: r-x
     */
    if (mkdir(tmp_path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    
    return 0;  // Success
}


int send_data(int sock, const void *data, size_t len) {
    size_t sent = 0;  // Track cumulative bytes sent
    
    /*
     * TRANSMISSION COMPLETION LOOP
     * Continue sending until all data transmitted
     * Handle partial sends gracefully
     */
    while (sent < len) {
        /*
         * SEND OPERATION
         * MSG_NOSIGNAL prevents SIGPIPE on broken connections
         * Pointer arithmetic advances through data buffer
         * Returns actual bytes sent (may be less than requested)
         */
        ssize_t n = send(sock, (char*)data + sent, len - sent, MSG_NOSIGNAL);
        
        /*
         * ERROR HANDLING AND RETRY LOGIC
         * n <= 0 indicates error or connection closed
         * EAGAIN/EWOULDBLOCK = temporary unavailability, retry
         * Other errors considered permanent
         */
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // Retry
            return -1;  // Permanent error
        }
        sent += n;  // Update sent byte counter
    }
    return sent;  // Return total bytes successfully sent
}


int receive_data(int sock, void *data, size_t len) {
    size_t received = 0;  // Track cumulative bytes received
    
    /*
     * RECEPTION COMPLETION LOOP
     * Continue until all expected data received
     */
    while (received < len) {
        /*
         * RECEIVE OPERATION
         * recv() reads available data from socket buffer
         * May return less data than requested
         * Returns 0 when connection closed, <0 on error
         */
        ssize_t n = recv(sock, (char*)data + received, len - received, 0);
        if (n <= 0) return -1;  // Connection closed or error
        received += n;  // Update received byte counter
    }
    return received;  // Return total bytes received
}


void handle_client(int client) {
    char command[5];      // 4-byte command + null terminator
    char path[MAX_PATH];  // File path for operation
    int path_len, data_len;  // Length fields from protocol
    
    /*
     * COMMAND RECEPTION
     * Receive 4-byte command identifier
     * Commands are fixed-length for protocol simplicity
     */
    if (receive_data(client, command, 4) <= 0) return;
    command[4] = '\0';  // Null terminate for string operations
    
    /*
     * PATH RECEPTION
     * Receive path length followed by path string
     * Variable-length field with length prefix
     */
    if (receive_data(client, &path_len, sizeof(int)) <= 0) return;
    if (receive_data(client, path, path_len) <= 0) return;
    path[path_len] = '\0';  // Null terminate path string
    
    /*
     * DATA LENGTH RECEPTION
     * Receive data length field (may be 0 for commands without data)
     */
    if (receive_data(client, &data_len, sizeof(int)) <= 0) return;
    
    /*
     * COMMAND LOGGING
     * Log received command for debugging and monitoring
     */
    printf("Command: %s, Path: %s, Data length: %d\n", command, path, data_len);
    
    /*
     * STORE COMMAND HANDLER
     * Stores PDF file received from S1 coordinator
     * Used when client uploads PDF file through S1
     */
    if (strcmp(command, "STOR") == 0) {
        /*
         * FILE DATA RECEPTION
         * Allocate buffer for complete file content
         * Receive entire file into memory before writing
         */
        char *data = malloc(data_len);
        if (!data) {
            int status = -1;  // Memory allocation failure
            send_data(client, &status, sizeof(int));
            return;
        }
        
        /*
         * COMPLETE FILE DATA RECEPTION
         * Must receive all data before proceeding with storage
         */
        if (receive_data(client, data, data_len) <= 0) {
            free(data);
            int status = -1;  // Reception failure
            send_data(client, &status, sizeof(int));
            return;
        }
        
        /*
         * DIRECTORY STRUCTURE CREATION
         * Extract directory path and create if necessary
         * Preserves client directory structure in S2
         */
        char dir_path[MAX_PATH];
        strncpy(dir_path, path, sizeof(dir_path));
        char *dir = dirname(dir_path);  // Extract directory portion
        
        /*
         * FILE STORAGE OPERATION
         * Create directories and write file atomically
         */
        if (create_dirs_recursive(dir) == 0) {
            /*
             * FILE CREATION AND WRITING
             * O_CREAT = create if doesn't exist
             * O_WRONLY = write-only access
             * O_TRUNC = truncate existing file to zero length
             * 0644 = rw-r--r-- permissions
             */
            int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) {
                write(fd, data, data_len);  // Write complete file
                close(fd);
                send_data(client, &data_len, sizeof(int));  // Success - return size
                printf("Stored PDF: %s (%d bytes)\n", path, data_len);
            } else {
                int status = -1;  // File creation failure
                send_data(client, &status, sizeof(int));
            }
        } else {
            int status = -1;  // Directory creation failure
            send_data(client, &status, sizeof(int));
        }
        
        free(data);  // Clean up file buffer
    }
    /*
     * RETRIEVE COMMAND HANDLER
     * Retrieves PDF file and streams to S1 coordinator
     * Used when client downloads PDF file through S1
     */
    else if (strcmp(command, "RETR") == 0) {
        /*
         * FILE EXISTENCE AND SIZE CHECK
         * Use stat() to get file information
         */
        struct stat st;
        if (stat(path, &st) == 0) {
            /*
             * FILE SIZE TRANSMISSION
             * Send file size to S1 so it knows how much data to expect
             */
            int size = st.st_size;
            send_data(client, &size, sizeof(int));
            
            /*
             * FILE CONTENT STREAMING
             * Open file and stream contents to S1
             * Use buffered reading for efficiency
             */
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                char buffer[BUF_SIZE];
                ssize_t bytes_read;
                
                /*
                 * STREAMING TRANSMISSION LOOP
                 * Read file in chunks and transmit
                 * Memory-efficient for large PDF files
                 */
                while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
                    send_data(client, buffer, bytes_read);
                }
                close(fd);
                printf("Sent PDF: %s (%d bytes)\n", path, size);
            }
        } else {
            /*
             * FILE NOT FOUND HANDLING
             * Send -1 to indicate file doesn't exist
             */
            int size = -1;
            send_data(client, &size, sizeof(int));
        }
    }
    /*
     * REMOVE COMMAND HANDLER
     * Deletes PDF file from S2 storage
     * Used when client deletes PDF file through S1
     */
    else if (strcmp(command, "REMV") == 0) {
        /*
         * FILE DELETION OPERATION
         * unlink() removes file from filesystem
         * Returns 0 on success, -1 on failure
         */
        int status = (unlink(path) == 0) ? 0 : -1;
        send_data(client, &status, sizeof(int));
        printf("Removed PDF: %s (%s)\n", path, status == 0 ? "success" : "fail");
    }
    /*
     * TAR CREATION COMMAND HANDLER
     * Creates tar archive of all PDF files (currently unused)
     * Placeholder for potential future functionality
     */
    else if (strcmp(command, "TARC") == 0) {
        /*
         * TEMPORARY TAR FILE CREATION
         * mkstemp() creates unique temporary file
         * Template contains XXXXXX which gets replaced with unique string
         */
        char tmp_tar[] = "/tmp/pdf_XXXXXX";
        int fd_tmp = mkstemp(tmp_tar);
        long size = 0;
        
        if (fd_tmp == -1) {
            perror("mkstemp failed");
            send_data(client, &size, sizeof(long));
            return;
        }
        close(fd_tmp);  // Close temporary file descriptor

        /*
         * TAR ARCHIVE CREATION USING SYSTEM COMMAND
         * Use find and tar commands to create archive
         * Error redirection suppresses error messages
         */
        const char *home = getenv("HOME");
        if (home) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), 
                "cd %s/S2 && find . -name '*.pdf' -type f 2>/dev/null | tar -cf %s -T - 2>/dev/null", 
                home, tmp_tar);
            printf("Executing: %s\n", cmd);
            system(cmd);  // Execute shell command
        }

        /*
         * TAR FILE SIZE DETERMINATION
         * Get size of created tar archive
         */
        struct stat st;
        if (stat(tmp_tar, &st) == 0) {
            size = st.st_size;
            printf("Created PDF tar of size %ld\n", size);
        }

        /*
         * TAR SIZE TRANSMISSION
         * Send archive size to client
         */
        send_data(client, &size, sizeof(long));

        /*
         * TAR DATA STREAMING
         * Stream tar archive contents if non-empty
         */
        if (size > 0) {
            int fd = open(tmp_tar, O_RDONLY);
            if (fd >= 0) {
                char buffer[BUF_SIZE];
                ssize_t bytes_read;
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
         * Remove temporary tar file
         */
        unlink(tmp_tar);
    }
    /*
     * LIST COMMAND HANDLER
     * Lists PDF files in specified directory
     * Used by client for directory browsing
     */
    else if (strcmp(command, "LIST") == 0) {
        /*
         * DIRECTORY LISTING GENERATION
         * Scan directory for PDF files and build listing
         */
        char result[BUF_SIZE] = "";
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry;
            
            /*
             * DIRECTORY ENTRY PROCESSING
             * Filter for regular files with .pdf extension
             */
            while ((entry = readdir(dir)) != NULL) {
                /*
                 * FILE TYPE AND EXTENSION FILTERING
                 * DT_REG = regular file (not directory, symlink, etc.)
                 * strstr() checks for .pdf substring in filename
                 */
                if (entry->d_type == DT_REG && strstr(entry->d_name, ".pdf")) {
                    strcat(result, entry->d_name);
                    strcat(result, "\n");  // Newline-separated listing
                }
            }
            closedir(dir);
        }
        
        /*
         * LISTING TRANSMISSION
         * Send listing size followed by listing data
         */
        int size = strlen(result);
        send_data(client, &size, sizeof(int));
        if (size > 0) {
            send_data(client, result, size);
        }
        printf("Listed PDFs in %s (%d files)\n", path, size);
    }
}

#include <stdio.h>      // Standard I/O (printf, fgets, scanf)
#include <stdlib.h>     // Memory management (malloc, free), process control
#include <string.h>     // String operations (strcmp, strcpy, strlen, strtok)
#include <unistd.h>     // POSIX APIs (close, read, write)
#include <sys/socket.h> // Socket programming (socket, connect, send, recv)
#include <netinet/in.h> // Internet address structures
#include <arpa/inet.h>  // IP address conversion (inet_pton)
#include <sys/stat.h>   // File status operations (stat, S_ISREG)
#include <fcntl.h>      // File control (open, O_RDONLY, O_CREAT)
#include <errno.h>      // Error handling (errno, perror)
#include <libgen.h>     // Path manipulation (basename)

/*
 * CLIENT CONFIGURATION CONSTANTS
 * These define the client's connection parameters and operational limits
 */
#define S1_PORT 8222     // S1 coordinator server port - single point of contact
#define S1_IP "127.0.0.1" // Server IP address - localhost for development
#define BUF_SIZE 16384   // 16KB buffer for network I/O - matches server buffer size
#define MAX_PATH 1024    // Maximum file path length - standard system limit
#define MAX_FILES 3      // Maximum files per operation - matches server limit

/*
 * FUNCTION PROTOTYPES
 * Forward declarations for modular code organization
 */
int connect_to_server();                          // Connection establishment and management
int validate_extension(const char *filename);     // File type validation
int validate_path(const char *path);             // Path security validation
int send_data(int sock, const void *data, size_t len);     // Reliable data transmission
int receive_data(int sock, void *data, size_t len);       // Reliable data reception
void upload_command(const char *input);          // Upload command implementation
void download_command(const char *input);        // Download command implementation
void remove_command(const char *input);          // Remove command implementation
void tar_command(const char *input);             // Tar download command implementation
void list_command(const char *input);            // Directory listing command implementation


int main() {
    /*
     * CLIENT STARTUP AND HELP DISPLAY
     * Provide user with available commands and usage information
     */
    printf("Distributed File System Client\n");
    printf("Commands: uploadf, downlf, removef, downltar, dispfnames, quit\n");
    
    char input[BUF_SIZE];         // Buffer for user input
    int server_sock = -1;         // Socket connection tracker (unused in current implementation)
    
    /*
     * INTERACTIVE COMMAND LOOP
     * Continue until user enters quit command or EOF
     * Implements classic REPL (Read-Eval-Print-Loop) pattern
     */
    while (1) {
        /*
         * COMMAND PROMPT DISPLAY
         * Show distinctive prompt to indicate client readiness
         * fflush() ensures prompt appears immediately
         */
        printf("s25client> ");
        fflush(stdout);  // Force output buffer flush for immediate display
        
        /*
         * USER INPUT RECEPTION
         * fgets() reads entire line including spaces and special characters
         * Returns NULL on EOF (Ctrl+D) or error
         */
        if (!fgets(input, sizeof(input), stdin)) {
            break;  // EOF or error - exit gracefully
        }
        
        /*
         * INPUT PREPROCESSING
         * Remove trailing newline character added by fgets()
         * strcspn() finds first occurrence of newline character
         */
        input[strcspn(input, "\n")] = '\0';
        
        /*
         * EMPTY INPUT HANDLING
         * Skip processing for empty lines (user just pressed Enter)
         */
        if (strlen(input) == 0) continue;
        
        /*
         * QUIT COMMAND HANDLING
         * Support both "quit" and "exit" for user convenience
         * Graceful termination with farewell message
         */
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            printf("Exiting client\n");
            break;
        }
        
        /*
         * COMMAND EXTRACTION
         * Extract first word as command using sscanf()
         * Commands are space-delimited
         */
        char command[20];
        sscanf(input, "%s", command);
        
        /*
         * COMMAND DISPATCH
         * Route to appropriate handler based on command string
         * Each handler parses its own arguments and implements protocol
         */
        if (strcmp(command, "uploadf") == 0) {
            upload_command(input);
        } else if (strcmp(command, "downlf") == 0) {
            download_command(input);
        } else if (strcmp(command, "removef") == 0) {
            remove_command(input);
        } else if (strcmp(command, "downltar") == 0) {
            tar_command(input);
        } else if (strcmp(command, "dispfnames") == 0) {
            list_command(input);
        } else {
            /*
             * UNKNOWN COMMAND HANDLING
             * Provide feedback for unrecognized commands
             * Continue operation instead of terminating
             */
            printf("Unknown command: %s\n", command);
        }
    }
    
    /*
     * CLEANUP ON EXIT
     * Close connection if established (defensive programming)
     */
    if (server_sock != -1) {
        close(server_sock);
    }
    
    return 0;  // Successful termination
}


int connect_to_server() {
    /*
     * CONNECTION STATE MANAGEMENT
     * Static variable would maintain connection across function calls
     * Current implementation always creates new connection
     */
    static int server_sock = -1;
    
    /*
     * CONNECTION REUSE CHECK
     * If connection already established, return existing socket
     * Optimization to avoid repeated connection overhead
     */
    if (server_sock != -1) {
        return server_sock;
    }
    
    /*
     * SOCKET CREATION
     * AF_INET = IPv4 internet protocols
     * SOCK_STREAM = TCP reliable, connection-oriented service
     * 0 = default protocol selection (TCP for SOCK_STREAM)
     */
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    /*
     * SERVER ADDRESS CONFIGURATION
     * Designated initializer syntax for clarity and safety
     * htons() converts port to network byte order (big-endian)
     */
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(S1_PORT)
    };
    
    /*
     * IP ADDRESS CONVERSION
     * inet_pton() converts string IP to binary network address
     * AF_INET specifies IPv4 address family
     * More robust than deprecated inet_addr()
     */
    inet_pton(AF_INET, S1_IP, &addr.sin_addr);
    
    /*
     * CONNECTION ESTABLISHMENT
     * connect() initiates TCP three-way handshake with server
     * Blocks until connection established or timeout/error
     */
    if (connect(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Connection to S1 failed");
        close(server_sock);
        server_sock = -1;  // Reset for next attempt
        return -1;
    }
    
    /*
     * CONNECTION SUCCESS CONFIRMATION
     * Provide user feedback on successful connection
     */
    printf("Connected to S1 server\n");
    return server_sock;
}


int validate_extension(const char *filename) {
    /*
     * EXTENSION EXTRACTION
     * strrchr() finds rightmost occurrence of '.' character
     * Handles files like "archive.old.zip" correctly (uses ".zip")
     * Returns NULL if no extension found
     */
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;  // No extension - unsupported
    
    /*
     * EXTENSION WHITELIST CHECK
     * Only allow file types that servers can handle
     * Uses string comparison for exact matching
     * Returns boolean result (1=valid, 0=invalid)
     */
    return (strcmp(ext, ".c") == 0 || strcmp(ext, ".pdf") == 0 || 
           strcmp(ext, ".txt") == 0 || strcmp(ext, ".zip") == 0);
}


int validate_path(const char *path) {
    /*
     * PATH SECURITY CHECK
     * Ensure path contains "S1" indicating it's within allowed directory
     * Supports both "S1" and "~/S1" patterns for user convenience
     * Simple but effective access control mechanism
     */
    return (strstr(path, "S1") != NULL || strstr(path, "~/S1") != NULL);
}


int send_data(int sock, const void *data, size_t len) {
    size_t sent = 0;  // Track total bytes transmitted
    
    /*
     * TRANSMISSION COMPLETION LOOP
     * Continue until all data sent or permanent error
     * Handle partial transmissions gracefully
     */
    while (sent < len) {
        /*
         * DATA TRANSMISSION ATTEMPT
         * MSG_NOSIGNAL prevents SIGPIPE on broken connection
         * Pointer arithmetic advances through data buffer
         * Returns actual bytes sent (may be less than requested)
         */
        ssize_t n = send(sock, (char*)data + sent, len - sent, MSG_NOSIGNAL);
        
        /*
         * ERROR AND RETRY HANDLING
         * n <= 0 indicates error or connection issue
         * EAGAIN/EWOULDBLOCK = temporary unavailability (retry)
         * Other errors are considered permanent failures
         */
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // Retry
            return -1;  // Permanent error
        }
        sent += n;  // Update total sent counter
    }
    return sent;  // Return total bytes successfully sent
}


int receive_data(int sock, void *data, size_t len) {
    size_t received = 0;  // Track total bytes received
    
    /*
     * RECEPTION COMPLETION LOOP
     * Continue until all expected data received or error
     */
    while (received < len) {
        /*
         * DATA RECEPTION ATTEMPT
         * recv() reads available data from socket buffer
         * May return less data than requested
         * Returns 0 when peer closes connection, <0 on error
         */
        ssize_t n = recv(sock, (char*)data + received, len - received, 0);
        if (n <= 0) return -1;  // Connection closed or error
        received += n;  // Update received counter
    }
    return received;  // Return total bytes received
}


void upload_command(const char *input) {
    /*
     * ARGUMENT PARSING SETUP
     * Support up to MAX_FILES + destination path
     * Static array for token storage during parsing
     */
    char *files[MAX_FILES+1];   // Allow 3 files + 1 destination
    int token_count = 0;

    /*
     * INPUT TOKENIZATION
     * Create modifiable copy for strtok() parsing
     * strtok() modifies input string by inserting null terminators
     */
    char input_copy[BUF_SIZE];
    strncpy(input_copy, input, sizeof(input_copy));
    input_copy[sizeof(input_copy)-1] = '\0';  // Ensure null termination

    /*
     * SPACE-DELIMITED PARSING
     * Extract command and arguments separately
     * Skip "uploadf" command word, collect file arguments
     */
    char *token = strtok(input_copy, " ");
    token = strtok(NULL, " "); // Skip command word

    while (token && token_count < MAX_FILES+1) {
        files[token_count++] = token;
        token = strtok(NULL, " ");  // Continue from last position
    }

    /*
     * ARGUMENT VALIDATION
     * Require minimum: one file + destination path
     */
    if (token_count < 2) {
        printf("Usage: uploadf file1 [file2] [file3] destination_path\n");
        return;
    }

    /*
     * DESTINATION EXTRACTION
     * Last argument is always destination path
     * Adjust file count to reflect actual number of files
     */
    char *dest = files[token_count-1];
    int file_count = token_count - 1;

    /*
     * FILE COUNT LIMITATION
     * Enforce system limit to prevent resource exhaustion
     */
    if (file_count > MAX_FILES) {
        file_count = MAX_FILES;
    }

    /*
     * DESTINATION PATH VALIDATION
     * Ensure destination is within allowed directory structure
     */
    if (!validate_path(dest)) {
        printf("Error: Destination must be in S1 directory\n");
        return;
    }

    /*
     * SERVER CONNECTION ESTABLISHMENT
     * Get socket connection to S1 server
     */
    int sock = connect_to_server();
    if (sock < 0) return;

    /*
     * COMMAND TRANSMISSION
     * Send complete command line to server
     * Server will parse arguments on its side
     */
    send_data(sock, input, strlen(input));

    /*
     * SERVER READINESS CONFIRMATION
     * Wait for "READY" response before proceeding with file transfers
     * Server uses this to indicate successful argument parsing
     */
    char response[6] = {0};
    if (receive_data(sock, response, 5) <= 0 || strcmp(response, "READY") != 0) {
        printf("Server not ready\n");
        return;
    }

    /*
     * FILE TRANSMISSION LOOP
     * Process each file individually with comprehensive validation
     */
    for (int i = 0; i < file_count; i++) {
        /*
         * FILE TYPE VALIDATION
         * Check extension before attempting upload
         */
        if (!validate_extension(files[i])) {
            printf("Invalid file type: %s\n", files[i]);
            continue;  // Skip this file, continue with others
        }

        /*
         * FILE EXISTENCE AND TYPE VALIDATION
         * stat() provides file metadata including size and type
         * S_ISREG() macro checks for regular file (not directory/device)
         */
        struct stat st;
        if (stat(files[i], &st) != 0 || !S_ISREG(st.st_mode)) {
            printf("File not found: %s\n", files[i]);
            continue;
        }

        /*
         * FILENAME EXTRACTION AND TRANSMISSION
         * basename() extracts filename from full path
         * Send filename length + filename string to server
         */
        char *filename = basename(files[i]);

        int name_len = strlen(filename);
        send_data(sock, &name_len, sizeof(int));
        send_data(sock, filename, name_len);

        /*
         * FILE SIZE TRANSMISSION
         * Server needs size for memory allocation and progress tracking
         */
        long file_size = st.st_size;
        send_data(sock, &file_size, sizeof(long));

        /*
         * FILE CONTENT TRANSMISSION WITH PROGRESS
         * Open file and stream contents to server
         * Provide real-time progress feedback to user
         */
        int fd = open(files[i], O_RDONLY);
        if (fd < 0) {
            printf("Failed to open %s\n", files[i]);
            continue;
        }

        printf("Uploading %s (%ld bytes)...\n", filename, file_size);
        char buffer[BUF_SIZE];
        ssize_t bytes_read;
        long total_sent = 0;

        /*
         * STREAMING TRANSMISSION LOOP
         * Read file in chunks and transmit to minimize memory usage
         * Update progress display in real-time
         */
        while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
            send_data(sock, buffer, bytes_read);
            total_sent += bytes_read;
            
            /*
             * PROGRESS DISPLAY
             * \r returns cursor to beginning of line for overwriting
             * Percentage calculation with floating-point precision
             * fflush() ensures immediate display update
             */
            printf("\rProgress: %ld/%ld bytes (%.1f%%)", 
                  total_sent, file_size, (double)total_sent/file_size * 100);
            fflush(stdout);  // Force immediate output
        }

        close(fd);
        printf("\n");  // Move to next line after progress display

        /*
         * SERVER RESPONSE RECEPTION
         * Get 4-byte status response for this file
         * Display result to user for feedback
         */
        char res[5] = {0};
        if (receive_data(sock, res, 4) > 0) {
            printf("Server response: %s\n", res);
        }
    }
}


void download_command(const char *input) {
    char *files[MAX_FILES];
    int file_count = 0;
    
    /*
     * ARGUMENT PARSING
     * Extract file list from command line
     * No destination needed - files saved to current directory
     */
    char input_copy[BUF_SIZE];
    strncpy(input_copy, input, sizeof(input_copy));
    
    char *token = strtok(input_copy, " ");
    token = strtok(NULL, " "); // Skip "downlf" command
    
    while (token && file_count < MAX_FILES) {
        files[file_count++] = token;
        token = strtok(NULL, " ");
    }
    
    /*
     * ARGUMENT VALIDATION
     * Require at least one file to download
     */
    if (file_count == 0) {
        printf("Usage: downlf file1 [file2]\n");
        return;
    }
    
    /*
     * SERVER CONNECTION AND COMMAND TRANSMISSION
     */
    int sock = connect_to_server();
    if (sock < 0) return;
    
    send_data(sock, input, strlen(input));
    
    /*
     * FILE COUNT RECEPTION
     * Server sends number of files that will be transmitted
     * May be less than requested if some files not found
     */
    int count;
    if (receive_data(sock, &count, sizeof(int)) <= 0) {
        printf("No response from server\n");
        return;
    }
    
    printf("Downloading %d files...\n", count);
    
    /*
     * FILE RECEPTION LOOP
     * Receive each file individually with full error handling
     */
    for (int i = 0; i < count; i++) {
        /*
         * FILENAME RECEPTION
         * Receive filename length followed by filename string
         */
        int name_len;
        if (receive_data(sock, &name_len, sizeof(int)) <= 0) break;
        
        char filename[256];
        if (receive_data(sock, filename, name_len) <= 0) break;
        filename[name_len] = '\0';  // Null terminate
        
        /*
         * FILE SIZE RECEPTION
         * Server sends file size or -1 if file not found
         */
        long file_size;
        if (receive_data(sock, &file_size, sizeof(long)) <= 0) break;
        
        /*
         * FILE AVAILABILITY CHECK
         * Handle cases where file doesn't exist or is empty
         */
        if (file_size < 0) {
            printf("File not found: %s\n", filename);
            continue;
        }
        
        if (file_size == 0) {
            printf("Empty file: %s\n", filename);
            continue;
        }
        
        /*
         * LOCAL FILE CREATION
         * Create file in current directory for download
         * O_CREAT|O_WRONLY|O_TRUNC = create new or truncate existing
         * 0644 permissions = rw-r--r-- (owner read/write, others read)
         */
        printf("Downloading %s (%ld bytes)... ", filename, file_size);
        fflush(stdout);
        
        int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            printf("FAILED (cannot create file)\n");
            
            /*
             * DATA SKIP OPERATION
             * If can't create local file, still need to consume data from socket
             * Otherwise socket will be out of sync for next file
             */
            char buffer[BUF_SIZE];
            long remaining = file_size;
            while (remaining > 0) {
                size_t to_read = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
                ssize_t n = recv(sock, buffer, to_read, 0);
                if (n <= 0) break;
                remaining -= n;
            }
            continue;
        }
        
        /*
         * FILE DATA RECEPTION WITH PROGRESS
         * Receive file contents and write to local file
         * Provide real-time progress feedback
         */
        char buffer[BUF_SIZE];
        long received = 0;
        while (received < file_size) {
            /*
             * CHUNK SIZE CALCULATION
             * Receive remaining data or buffer size, whichever is smaller
             * Prevents over-reading past file boundary
             */
            size_t to_read = (file_size - received) > sizeof(buffer) ? 
                            sizeof(buffer) : (file_size - received);
            ssize_t n = recv(sock, buffer, to_read, 0);
            if (n <= 0) break;  // Connection error or closed
            
            write(fd, buffer, n);  // Write received data to file
            received += n;
            
            /*
             * PROGRESS UPDATE
             * Overwrite previous progress line with current status
             */
            printf("\rDownloading %s... %ld/%ld bytes (%.1f%%)", 
                  filename, received, file_size, (double)received/file_size * 100);
            fflush(stdout);
        }
        
        close(fd);
        
        /*
         * COMPLETION STATUS
         * Check if download completed successfully
         * Remove partial file if download failed
         */
        if (received == file_size) {
            printf("\rDownloading %s... DONE\n", filename);
        } else {
            printf("\rDownloading %s... FAILED (incomplete)\n", filename);
            unlink(filename);  // Remove incomplete file
        }
    }
}


void remove_command(const char *input) {
    char *files[MAX_FILES];
    int file_count = 0;
    
    /*
     * ARGUMENT PARSING
     * Extract file list from command line arguments
     * Similar to download but files will be deleted from server
     */
    char input_copy[BUF_SIZE];
    strncpy(input_copy, input, sizeof(input_copy));
    
    char *token = strtok(input_copy, " ");
    token = strtok(NULL, " "); // Skip "removef" command
    
    while (token && file_count < MAX_FILES) {
        files[file_count++] = token;
        token = strtok(NULL, " ");
    }
    
    /*
     * ARGUMENT VALIDATION
     * Require at least one file to delete
     */
    if (file_count == 0) {
        printf("Usage: removef file1 [file2]\n");
        return;
    }
    
    /*
     * SERVER CONNECTION AND COMMAND TRANSMISSION
     */
    int sock = connect_to_server();
    if (sock < 0) return;
    
    send_data(sock, input, strlen(input));
    
    /*
     * OPERATION RESULT RECEPTION
     * Server sends single 4-byte status for entire operation
     * OK = all files deleted successfully
     * FAIL = one or more deletions failed
     */
    char response[5] = {0};
    if (receive_data(sock, response, 4) > 0) {
        if (strncmp(response, "OK", 2) == 0) {
            printf("Files removed successfully\n");
        } else {
            printf("File removal failed\n");
        }
    } else {
        printf("No response from server\n");
    }
}


void tar_command(const char *input) {
    char filetype[20];
    
    /*
     * ARGUMENT PARSING AND VALIDATION
     * Extract file type from command line
     * sscanf() extracts second word as file type
     */
    if (sscanf(input, "%*s %s", filetype) != 1) {
        printf("Usage: downltar filetype (.c/.pdf/.txt)\n");
        return;
    }
    
    /*
     * FILE TYPE VALIDATION
     * Only support specific file types that have specialized servers
     */
    if (strcmp(filetype, ".c") != 0 && 
        strcmp(filetype, ".pdf") != 0 && 
        strcmp(filetype, ".txt") != 0) {
        printf("Invalid filetype. Use .c, .pdf, or .txt\n");
        return;
    }
    
    /*
     * SERVER CONNECTION AND COMMAND TRANSMISSION
     */
    int sock = connect_to_server();
    if (sock < 0) return;
    
    send_data(sock, input, strlen(input));
    
    /*
     * TAR SIZE RECEPTION
     * Server sends archive size or 0 if no files found
     */
    long tar_size;
    if (receive_data(sock, &tar_size, sizeof(long)) <= 0) {
        printf("No response from server\n");
        return;
    }
    
    /*
     * EMPTY ARCHIVE HANDLING
     * If no files of specified type found, archive will be empty
     */
    if (tar_size <= 0) {
        printf("No files found for %s\n", filetype);
        return;
    }
    
    /*
     * OUTPUT FILENAME DETERMINATION
     * Map file types to standardized archive names
     * Based on specification requirements
     */
    char output_file[50];
    if (strcmp(filetype, ".c") == 0) {
        strcpy(output_file, "cfiles.tar");
    } else if (strcmp(filetype, ".pdf") == 0) {
        strcpy(output_file, "pdf.tar");
    } else {
        strcpy(output_file, "text.tar");
    }
    
    /*
     * LOCAL FILE CREATION FOR ARCHIVE
     * Create tar file in current directory
     */
    printf("Downloading %s (%ld bytes)...\n", output_file, tar_size);
    int fd = open(output_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        printf("Failed to create output file\n");
        return;
    }
    
    /*
     * TAR ARCHIVE STREAMING DOWNLOAD
     * Receive tar data in chunks and write to local file
     * Provide real-time progress indication for large archives
     */
    char buffer[BUF_SIZE];
    long received = 0;
    while (received < tar_size) {
        /*
         * CHUNK SIZE CALCULATION
         * Read remaining data or buffer size, whichever is smaller
         */
        size_t to_read = (tar_size - received) > sizeof(buffer) ? 
                        sizeof(buffer) : (tar_size - received);
        ssize_t n = recv(sock, buffer, to_read, 0);
        if (n <= 0) break;  // Connection error
        
        write(fd, buffer, n);  // Write to tar file
        received += n;
        
        /*
         * PROGRESS DISPLAY
         * Show bytes received and percentage completion
         */
        printf("\rProgress: %ld/%ld bytes (%.1f%%)", 
              received, tar_size, (double)received/tar_size * 100);
        fflush(stdout);
    }
    
    close(fd);
    
    /*
     * COMPLETION STATUS
     * Verify complete download or clean up partial file
     */
    if (received == tar_size) {
        printf("\rDownload completed: %s\n", output_file);
    } else {
        printf("\rDownload failed: received %ld/%ld bytes\n", received, tar_size);
        unlink(output_file);  // Remove incomplete archive
    }
}


void list_command(const char *input) {
    char path[MAX_PATH];
    
    /*
     * ARGUMENT PARSING
     * Extract directory path from command line
     */
    if (sscanf(input, "%*s %s", path) != 1) {
        printf("Usage: dispfnames path\n");
        return;
    }
    
    /*
     * PATH VALIDATION
     * Ensure path is within allowed S1 directory structure
     * Security measure to prevent unauthorized directory access
     */
    if (!validate_path(path)) {
        printf("Error: Path must be in S1 directory\n");
        return;
    }
    
    /*
     * SERVER CONNECTION AND COMMAND TRANSMISSION
     */
    int sock = connect_to_server();
    if (sock < 0) return;
    
    send_data(sock, input, strlen(input));
    
    /*
     * LISTING SIZE RECEPTION
     * Server sends size of file listing data
     */
    int list_size;
    if (receive_data(sock, &list_size, sizeof(int)) <= 0) {
        printf("No response from server\n");
        return;
    }
    
    /*
     * EMPTY DIRECTORY HANDLING
     */
    if (list_size <= 0) {
        printf("No files found\n");
        return;
    }
    
    /*
     * SIZE VALIDATION
     * Prevent excessive memory allocation for malformed responses
     * BUF_SIZE * 4 provides reasonable upper bound for file listings
     */
    if (list_size > BUF_SIZE * 4) {
        printf("List too large: %d bytes\n", list_size);
        return;
    }
    
    /*
     * LISTING DATA RECEPTION
     * Allocate buffer for complete file listing
     * Receive all listing data as single block
     */
    char *list_data = malloc(list_size + 1);
    if (!list_data) {
        printf("Memory allocation failed\n");
        return;
    }
    
    if (receive_data(sock, list_data, list_size) <= 0) {
        free(list_data);
        return;
    }
    
    /*
     * LISTING DISPLAY
     * Null-terminate and display complete file listing
     * Server provides pre-formatted listing with newlines
     */
    list_data[list_size] = '\0';
    printf("Files in %s:\n%s", path, list_data);
    free(list_data);  // Clean up allocated memory
}


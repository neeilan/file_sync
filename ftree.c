#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <libgen.h>

#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/types.h>         
#include <sys/socket.h>
#include <arpa/inet.h>
#include <limits.h>
       
#include "ftree.h"
#include "hash.h"

#define MAX_BACKLOG 5



// ================================ PROTOTYPES ================================

/*
 * Data structure for storing information about a single file.
 * For dirs, contents is the linked list of files in the dir and hash is NULL.
 * For files, contents is NULL, and the hash is the hash of the file.
 * next is the next file in the directory (or NULL).
 */
struct TreeNode
{
    char *fname;
    char *abs_dest_path;
    int mode;
    int size;
    char *hash;                  // For normal files and links
    struct TreeNode *contents;   // For directories
    struct TreeNode *next;
};


// Client

void add_hash(struct TreeNode* node, const char* filePath);
int sync_ftree(struct TreeNode* root, int fd);

struct TreeNode *generate_ftree(char *fname, char* abs_dest_path);
struct fileinfo node_to_fileinfo(struct TreeNode* file_node);
void add_properties(
    struct TreeNode *node,
    const char* fname,
    char* abs_dest_path, 
    int size, int mode);
    
int sync_file(struct fileinfo* file, int fd,  char* path_in_dest); 
int transfer_file(struct fileinfo* file, int fd); 
int server_status(int fd);


// Server

int send_status(int fd, int response_code);
int directory_file_helper (int fd, struct fileinfo* file);
int check_hash (struct fileinfo* client_file);
int read_fileinfo(int fd, struct fileinfo* file);
int handle_file(int fd);
void create_file (int fd, struct fileinfo* client_file);
int diff_types(int mode_f1, int mode_f2);


// Miscellaneous
int free_ftree (struct TreeNode* root);
int read_bytes(int fd, int num_bytes, void* buffer);
char* join_path(const char *parentPath, const char *childName);


// ================================== CLIENT ==================================

/*
 * Copies file at src_path on client into dest_path on server at host, running
 * on port
 */
int fcopy_client(char *src_path, char *dest_path, char *host, int port) {
    
    // Setup connection to the server
    // 1) File descriptor
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);    

    // 2) Setting up the sockaddr_in to connect to
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);    
    
    if (inet_pton(AF_INET, host, &server.sin_addr)< 0) {
        perror("client: inet_pton");
        close(sock_fd);
        exit(1);
    }    

    // 3) Connect!
    if (connect(sock_fd, (struct sockaddr *) &server, sizeof(server)) == -1) {
        perror("client: connect");
        close(sock_fd);
        exit(1);
    }
    
    struct TreeNode* root = generate_ftree(src_path, dest_path);
    if (sync_ftree(root, sock_fd) == -1) {
        close(sock_fd);
        free_ftree(root);
        exit(1);
    }
    close(sock_fd);
    free_ftree(root);

    return 0;
}

/*
 * Frees malloc'd memory for the ftree rooted at root
 */
int free_ftree (struct TreeNode* root) {
    if (!root)
       return -1;
    
    if (root->hash == NULL) {
        free(root->fname);
        free(root->abs_dest_path);
    }

    struct TreeNode* curr_node = root->contents;
    struct TreeNode* prev_node;           // for memory deallocation
    
    while (curr_node != NULL) {
        prev_node = curr_node;
        if (curr_node->hash == NULL) {    // directory
            free_ftree(curr_node);
        } else {
          free(curr_node->fname);
          free(curr_node->abs_dest_path);
          free(curr_node->hash);    
        }
      
      curr_node = curr_node->next;
      free(prev_node);
    }

    return 0;
}


/*
 * Return -1 if error, 0 otherwise
 */
int sync_ftree (struct TreeNode* root, int fd)
{
    if (!root) return -1;
    
    char dir_path_in_dest[MAXPATH];
    strcpy(dir_path_in_dest, root->abs_dest_path);

    // Sync the file
    struct fileinfo file_or_dir = node_to_fileinfo(root);
    if (sync_file (&file_or_dir, fd, dir_path_in_dest) == -1)
        return -1;

    struct TreeNode* curr_node = root->contents;

    while (curr_node != NULL) {
        if (curr_node->hash == NULL) { 
            // directory
            sync_ftree(curr_node, fd);
        } else {
           char* regfile_path_in_dest = calloc(MAXPATH, sizeof(char));
           strcpy(regfile_path_in_dest, curr_node->abs_dest_path);  
           struct fileinfo file = node_to_fileinfo(curr_node);
        
           if (sync_file(&file, fd, regfile_path_in_dest) == -1)
               return -1;

           free(regfile_path_in_dest);
        }
      
      curr_node = curr_node->next;
    } 
   
    return 0;
}

/*
 * Sync file with the file path_in_dest on server, via socket fd
 */
int sync_file(struct fileinfo* file, int fd, char* path_in_dest)
{
    
    // Write the struct in the order path, mode, hash, size
    unsigned long nmode = htonl(file->mode);
    unsigned long nsize = htonl(file->size);
    
    // Want to write the path in dest, not local path
    write(fd, path_in_dest, sizeof(char) * MAXPATH);
    write(fd, &nmode, sizeof(mode_t));
    write(fd, (file->hash), sizeof(char) * HASH_SIZE);
    write(fd, &nsize, sizeof(size_t));
    
    int status = server_status(fd);

    if (status == MISMATCH) {
        if ((int)(file->size) > 0) {
            int transfer_result = transfer_file(file, fd);
            if (transfer_result == -1)
                return -1;
        }
        
        int transmit_status = server_status(fd);
        
        if ( transmit_status == TRANSMIT_ERROR ) {
            printf("TRANSMIT ERROR: %s\n", file->path);
            return -1;
        }
        
    } else if (status == MATCH_ERROR) {
        printf("MATCH ERROR: %s\n", file->path); 
        return -1;
    } else {
        printf("UNKNOWN RESPONSE\n");
        return -1;
    }  
    
    return 0;    
}


/*
 * Transfer file represented by fileinfo file, via socket fd
 */
int transfer_file(struct fileinfo* file, int fd)
{
    FILE *src_file = fopen(file->path, "r");
    char c;
    int num_transferred = 0;

    if (src_file == NULL) {
        perror("fopen");
        return -1;
    }
    
    do 
    {
      c = fgetc(src_file);
      write(fd, &c, sizeof(char));
      num_transferred++;
    } while ( num_transferred < file->size );  
    
    fclose(src_file);
    return 1;
}


// ============================== CLIENT HELPERS ==============================

/*
 *  Returns the FTree rooted at the path fname.
 */
struct TreeNode *generate_ftree(char *fname, char* abs_dest_path)
{
    struct stat sb;
    DIR* p_dir;
    struct dirent* entry;
    char* joined_path;
    
    struct TreeNode* root = malloc(sizeof(struct TreeNode));
    root->size = 0;
    root->hash = NULL;
    root->hash = NULL;
    root->contents = NULL;
    root->next = NULL;
    root->fname = NULL;

    if (lstat(fname, &sb) == -1) {
        perror("lstat: ");
        exit(-1);
    }
    
    // add basic properties
     add_properties(root, fname, abs_dest_path, sb.st_size, sb.st_mode);

    int type = sb.st_mode & S_IFMT;
 
    if (type == S_IFDIR) {
        root->hash = NULL;
        p_dir = opendir(fname);
        
        if (p_dir == NULL) {
            perror("opendir");
            return NULL;
        }
        
        while ((entry = readdir(p_dir)) != NULL) {
            if (strncmp(entry->d_name, ".", 1) == 0)
                continue;

            joined_path = join_path(fname, entry->d_name);
            if (root->contents == NULL) {
                root->contents = generate_ftree(
                    joined_path,
                    root->abs_dest_path);
                free(joined_path);

            } else {
                struct TreeNode* old_head = root->contents;
                root->contents = generate_ftree(joined_path, root->abs_dest_path);
                free(joined_path);   
                (root->contents)->next = old_head;
            }
        }
        closedir(p_dir);
    } else if ( type == S_IFREG ) { // regular file  
        add_hash(root, fname);
    }
    
    return root;
}


/*
 * Transfers data from Treenode into a struct fileinfo
 */
struct fileinfo node_to_fileinfo(struct TreeNode *file_node)
{
    struct fileinfo f;

    f.mode = file_node->mode;
    f.size = file_node->size;
    strcpy(f.path, file_node->fname);
    
    if (file_node->hash)
        strcpy(f.hash, file_node->hash);

    return f;
}

/*
 * Adds basic properties for node
 */
void add_properties(
    struct TreeNode* node,
    const char* fname, 
    char* abs_dest_path, 
    int size, 
    int mode)
{
    node->fname = malloc(MAXPATH * (sizeof(char)));
    strncpy(node->fname, fname, MAXPATH);
    
    char* real = strdup(fname); 
    
    node->abs_dest_path = join_path(abs_dest_path, basename(real));
    
    free(real);
    
    node->mode = mode;
    node->size = size;
}

/*
 * Adds the hash property to node
 */
void add_hash(struct TreeNode* node, const char* filePath) 
{
    FILE *file = fopen(filePath, "r");
    if(file) {
        node->hash = hash(file);
        fclose(file);    
    }
}


/*
 *  Reads and returns the latest status code (MATCH, MISMATCH etc) from fd
 */
int server_status(int fd)
{
    unsigned long server_response;
    read_bytes(fd, sizeof(unsigned long), &server_response);        
    return ntohl(server_response);   
}


// ================================== SERVER ==================================

/*
 * Starts server, listening on port
 */
void fcopy_server(int port)
{
    // Create socket
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("server: socket");
        exit(1);
    }

    // Bind socket to an address
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);        // Note use of htons here
    server.sin_addr.s_addr = INADDR_ANY;
    memset(&server.sin_zero, 0, 8);       // Initialize sin_zero to 0

    int bind_res = bind(
        sock_fd, 
        (struct sockaddr *)&server,
        sizeof(struct sockaddr_in)
        );
        
    if (bind_res < 0) {
        perror("server: bind");
        close(sock_fd);
        exit(1);
    }

    int client_fd;
    struct sockaddr_in peer;
    int socklen = sizeof(peer);
    
    if (listen(sock_fd, MAX_BACKLOG) < 0) {
        perror("server: listen");
        close(sock_fd);
        exit(1);
    }

    while (1) {
        client_fd = accept(
            sock_fd, 
            (struct sockaddr *) &peer, 
            (socklen_t *) 
            &socklen);
        
        while(handle_file(client_fd) >= 0)
            ;
            
        close(client_fd);
    }
    close(sock_fd);
}

// -1 = error. 0 = ok.
int handle_file(int fd)
{
    struct fileinfo file;
    
    if (read_fileinfo(fd, &file) == -1) {
        // Error - fileinfo couldn't be read properly
        return -1;
    }

    //first check if the file even exists on the server side using lstat
    //if not, it needs to be created (use helpers)
    //after creation, check if the hashes match
    //struct passed in (use a helper)
    
    struct stat stat_server;
    if (lstat(file.path, &stat_server) == -1) {
        return directory_file_helper (fd, &file);    
    } else if (diff_types(file.mode, stat_server.st_mode)) {
        send_status(fd, MATCH_ERROR);
        fprintf(stderr, "%s", "MISMATCH in Filetypes\n");
        return -1;
    } else {    
        switch (file.mode & S_IFMT) {
            // we just need to change the permissions of this is the case
            case S_IFDIR: {
                send_status(fd, MATCH);
                chmod(file.path, file.mode);
                return 0;
                break;
            }
            case S_IFREG: 
            {
                int hash_match = check_hash(&file);

                if (hash_match == 1) {
                    send_status(fd, MATCH);
                    chmod(file.path, file.mode);  
                    return 0;
                } else {
                    //send a response to client that file needs to copied
                    send_status(fd, MISMATCH);

                    create_file(fd, &file);
                    chmod(file.path, file.mode);

                    hash_match = check_hash(&file);
                    if (!hash_match) {
                        send_status(fd, TRANSMIT_ERROR);
                        return -1;
                    } else {
                        send_status(fd, TRANSMIT_OK);
                        return 0;
                    }
                }
                break;
            }
            default : {
                printf("Unknown file: %s\n", file.path);
                return -1;
            }
        }
    }
}


// ============================== SERVER HELPERS ==============================

//if file/dir needs to be created on server
// -1 = error. 0 = ok.
int directory_file_helper (int fd, struct fileinfo* client_file)
{
    switch (client_file->mode & S_IFMT)
    {
        /* 
         * create a folder and update permissions
         * then send appropiate MATCH/MATCH_ERROR to client
         */
        case S_IFDIR: 
        {
            if (mkdir(client_file->path, client_file->mode) == -1) {
                perror("mkdir: ");
                send_status(fd, MATCH_ERROR); 
                return -1;
            } else {
                send_status(fd, MATCH); 
                return 0;
            }
        }

        /*
         * create a file + check hash to ensure proper creation
         */
         
        case S_IFREG: {
            // let client know that a file needs to be sent MISMATCH 
            send_status(fd, MISMATCH);
            create_file(fd, client_file);
            chmod(client_file->path, client_file->mode);
            int hash_match = check_hash(client_file);

            if (!hash_match) {
                send_status(fd, TRANSMIT_ERROR);
                char *problemFile = basename(strdup(client_file->path));
                printf("TRANSMIT ERROR: %s\n", problemFile); 
                return -1;
            } else {
                send_status(fd, TRANSMIT_OK);
                return 0;
            }
        }
        default : {
            return -1;
        }
    }
}

/*
 * Transfers response_code through socket descriptor fd
 */
int send_status(int fd, int response_code)
{
    unsigned long n_code = htonl(response_code);
    write(fd, &n_code, sizeof(unsigned long));
    return 1;
}


/*
 * Populates fields of file by reading from socket fd, in the order
 * path, mode, hash, size
 */
int read_fileinfo(int fd, struct fileinfo* file)
{

    if (read_bytes(fd, MAXPATH, file->path) == -1)
        return -1;
    if (read_bytes(fd, sizeof(mode_t), &(file->mode)) == -1)
        return -1;
    if (read_bytes(fd, HASH_SIZE, file->hash) == -1)
        return -1;
    if (read_bytes(fd, sizeof(size_t), &(file->size)) == -1)
        return -1;

    file->mode = ntohl(file->mode);
    file->size = ntohl(file->size);

    return 1;
}


// helper function - creates file, given an fileinfo
void create_file (int fd, struct fileinfo* client_file)
{
    FILE *file = fopen(client_file->path, "w");
    
    if (file == NULL) {
        perror("fopen");
        return;       
    }
    
    int bytes_currently_read = 0;
    int nbytes = 0;
    char buffer[1];

    while((int)(client_file->size) != bytes_currently_read) {
        nbytes = read(fd, buffer, 1);
        bytes_currently_read = bytes_currently_read + nbytes;
        fwrite(&buffer, sizeof(char), 1, file);
    }
    
    fclose(file);
}


/*
 * Returns 1 if server's file hash equals client's, 0 otherwise.
 */
int check_hash (struct fileinfo* client_file)
{
    char *server_file_hash;
    FILE *file_contents_server = fopen(client_file->path, "r");
    server_file_hash = hash(file_contents_server);
    fclose(file_contents_server);
    
    if (strncmp (server_file_hash, client_file->hash, HASH_SIZE) != 0) {
        free(server_file_hash);
        return 0;
    } else {
        free(server_file_hash);
        return 1;
    }
}


/*
 * Return 1 iff the files for the two modes are of different types (dir/file)
 * Else return 0.
 */
int diff_types(int mode_1, int mode_2)
{
    return (((mode_1 & S_IFMT) ==  S_IFREG) && ((mode_2 & S_IFMT) == S_IFDIR)) 
    || (((mode_1 & S_IFMT) ==  S_IFDIR)  && ((mode_2 & S_IFMT) == S_IFREG));   
}


// ============================== MISC HELPERS ==============================

/*
 *  Returns a new string consisting of parent and child paths joined with /
 */
char* join_path(const char *parent_path, const char *child_name) 
{
    char *result = malloc(
        sizeof(char) * (strlen(parent_path) + strlen(child_name) + 2)
        ); //+2 for the zero-terminator and /
        
    strcpy(result, parent_path);
    strcat(result, "/");
    strcat(result, child_name);
    return result;
}


/*
 * Reads the next num_bytes bytes from the stream fd and stores them into buffer
 * 0 if ok, -1 if fd closed
 */
int read_bytes(int fd, int num_bytes, void* buffer)
{
    int num_read = 0;
    int curr_read = 0;

    do {
        curr_read = read(fd, buffer + num_read, num_bytes);
        num_read += curr_read;
        if (curr_read <= 0) return -1;
    } while (num_bytes - num_read > 0);

    return 0;
}

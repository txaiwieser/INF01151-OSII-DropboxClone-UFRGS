#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/inotify.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#define __USE_XOPEN
#include <utime.h>
#include <unistd.h>
#include <netdb.h>
#include <libgen.h>
#include <pthread.h>
#include <math.h>
#include <stdlib.h>
#include <dirent.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "../include/dropboxClient.h"

char server_host[256], server_user[MAXNAME], user_sync_dir_path[256];
int server_port = 0, sock = 0;
FILE_INFO_t client_files[MAXFILES]; // List of client files
uint16_t sync_files_left = MAXFILES, original_sync_files_left = MAXFILES;  // number of files yet to sync
char *pIgnoredFileEntry; // Pointer to list of ignored files. It's used by inotify to prevent uploading a file that has just best downloaded.
int inotify_run = 0;
SSL *ssl, *ssl_cls; // TODO conferir se ta certo

pthread_barrier_t syncbarrier;
pthread_barrier_t serverlistenerbarrier;

pthread_mutex_t fileOperationMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t inotifyMutex = PTHREAD_MUTEX_INITIALIZER;

TAILQ_HEAD(, tailq_entry) ignoredfiles_tailq_head;

int getLogicalTime(void){
    time_t t0, t1, ts, tc;
    char buffer[MSGSIZE] = {0};

    // Sets timestamp T0
    t0 = time(NULL);
    // Gets server logical time
    SSL_write(ssl, "TIME", MSGSIZE);
    SSL_read(ssl, buffer, MSGSIZE);
    ts = atol(buffer); // TODO atoi?
    // Sets timestamp T1
    t1 = time(NULL);
    // Calculate client time
    tc = ts + (t1 - t0)/2;

    return tc;
}

void send_file(char *file) {
    struct stat st;
    char buffer[MSGSIZE] = {0}, method[MSGSIZE];
    int valread, file_i, file_found = -1, first_free_index = -1;
    time_t file_mtime;

    pthread_mutex_lock(&fileOperationMutex);

    if (stat(file, &st) == 0 && S_ISREG(st.st_mode)) { // If file exists
        FILE *fp = fopen(file,"rb");
        if (fp == NULL) {
            printf("Couldn't open the file\n");
        } else {
            // Concatenate strings to get method = "UPLOAD filename"
            sprintf(method, "UPLOAD %s", basename(file));

            // Call the server
            SSL_write(ssl, method, sizeof(method));
            debug_printf("[%s method sent]\n", method);

            sprintf(buffer, "%d", getLogicalTime()); // TODO integer? long?
            debug_printf("buffer = %d\n", buffer);
            SSL_write(ssl, buffer, MSGSIZE);
            file_mtime = atoi(buffer);

            debug_printf("vou ler\n");

            // Search for file in user's list of files
            for(file_i = 0; file_i < MAXFILES; file_i++) {
                if(strcmp(client_files[file_i].name, basename(file)) == 0) {
                    file_found = file_i;
                    break;
                }
            }

            // Find the next free slot in user's list of files
            for(file_i = 0; file_i < MAXFILES; file_i++) {
                if(client_files[file_i].size == FREE_FILE_SIZE && first_free_index == -1)
                    first_free_index = file_i;
            }

            // If file already exists on client file list, update it. Otherwise, insert it.
            if(file_found >= 0) {
                client_files[file_found].size = st.st_size;
                client_files[file_found].last_modified = file_mtime;
            } else {
                debug_printf("basename(file): %s", basename(file));
                strcpy(client_files[first_free_index].name, basename(file));
                client_files[first_free_index].size = st.st_size;
                client_files[first_free_index].last_modified = file_mtime;
            }

            // Detect if file was created and local version is newer than server version, so file must be transfered
            valread = SSL_read(ssl, buffer, MSGSIZE);
            debug_printf("leuuuuu\n");
            if (strncmp(buffer, TRANSMISSION_CONFIRM, TRANSMISSION_MSG_SIZE) == 0) {
                debug_printf("ooooo\n");
                // Send file size to server
                sprintf(buffer, "%d", st.st_size); // TODO integer? long?
                SSL_write(ssl, buffer, MSGSIZE);

                // Read data from file and send it
                while (1) {
                    // First read file in chunks of 1024 bytes
                    unsigned char buff[MSGSIZE] = {0};
                    int nread = fread(buff, 1, sizeof(buff), fp);
                    buff[nread] = '\0';

                    // If read was success, send data.
                    if (nread > 0) {
                        SSL_write(ssl, buff, MSGSIZE);
                    }

                    // Either there was error, or reached end of file
                    if (nread < sizeof(buff)) {
                        /*if (feof(fp))
                            debug_printf("End of file\n"); */
                        if (ferror(fp)) {
                            printf("Error reading\n");
                        }
                        break;
                    }
                }
            }
        }
        fclose(fp);
    } else {
        printf("File doesn't exist! Pass a valid filename.\n");
    }

    pthread_mutex_unlock(&fileOperationMutex);
}

void get_file(char *file, char *path) {
    int valread, file_i, file_found = -1, first_free_index = -1, file_size;
    uint32_t nLeft;
    time_t original_file_time;
    char buffer[MSGSIZE] = {0}, method[MSGSIZE], file_path[256];
    struct utimbuf new_times;
    struct tailq_entry *ignoredfile_node;

    pthread_mutex_lock(&fileOperationMutex);

    // If current directory is user_sync_dir, overwrite the file. It's another directory, get file only if there's no file with same filename
    if(!file_exists(file) || strcmp(user_sync_dir_path, path) == 0) {

        // Set saving path to current directory
        sprintf(file_path, "%s/%s", path, file);

        // Concatenate strings to get method = "DOWNLOAD filename"
        sprintf(method, "DOWNLOAD %s", file);

        // Send to the server
        SSL_write(ssl, method, sizeof(method));
        debug_printf("[%s method sent]\n", method);

        // Detect if server accepted transmission
        valread = SSL_read(ssl, buffer, MSGSIZE);
        if (strncmp(buffer, TRANSMISSION_CONFIRM, TRANSMISSION_MSG_SIZE) == 0) {
          // Create file where data will be stored
          FILE *fp;
          fp = fopen(file_path, "wb");
          if (NULL == fp) {
              printf("Error opening file");
          } else {
              // Receive file size
              SSL_read(ssl, buffer, MSGSIZE);
              nLeft = atoi(buffer); // TODO nao usar atoi?
              file_size = nLeft;

              // Receive data in chunks
              while (nLeft > 0 && (valread = SSL_read(ssl, buffer, sizeof(buffer))) > 0) {
                  fwrite(buffer, 1, MIN(nLeft,valread), fp);
                  nLeft -= MIN(nLeft,valread);
              }
              if (valread < 0) {
                  printf("\n Read Error \n");
              }
          }

          SSL_read(ssl, buffer, MSGSIZE);
          original_file_time = atol(buffer); // TODO nao usar atoi? atol? outras funcoes mais seguras?

          // Search for file in user's list of files
          for(file_i = 0; file_i < MAXFILES; file_i++) {
              if(strcmp(client_files[file_i].name, file) == 0) {
                  file_found = file_i;
                  break;
              }
          }

          // Find the next free slot in user's list of files
          for(file_i = 0; file_i < MAXFILES; file_i++) {
              if(client_files[file_i].size == FREE_FILE_SIZE && first_free_index == -1)
                  first_free_index = file_i;
          }

          // If file already exists on client file list, update it. Otherwise, insert it.
          if(file_found >= 0) {
              client_files[file_found].size = file_size;
              client_files[file_found].last_modified = original_file_time;
          } else {
              strcpy(client_files[first_free_index].name, file);
              client_files[first_free_index].size = file_size;
              client_files[first_free_index].last_modified = original_file_time;
          }

          pthread_mutex_lock(&inotifyMutex); // prevent inotify of getting file before it's inserted in ignored file list
          fclose (fp);

          // Set modtime to original file modtime
          new_times.modtime = original_file_time; // set mtime to original file time
          new_times.actime = time(NULL); // set atime to current time
          utime(file_path, &new_times);

          // If path is user_sync_dir, insert filename in the list of ignored files
          if(strcmp(user_sync_dir_path, path) == 0) {
              ignoredfile_node = malloc(sizeof(*ignoredfile_node));
              strcpy(ignoredfile_node->filename, file);
              if (ignoredfile_node == NULL) {
                  perror("malloc failed");
              }
              TAILQ_INSERT_TAIL(&ignoredfiles_tailq_head, ignoredfile_node, entries);
              debug_printf("[Added %s to list of ignored files because it has just been downloaded.]\n", file);
          }
          pthread_mutex_unlock(&inotifyMutex);
        } else {
          printf("Invalid filename\n");
        }
    } else {
        printf("There's already a file named %s in this directory\n", file);
    }
    pthread_mutex_unlock(&fileOperationMutex);
};

void delete_server_file(char *file) {
    char method[MSGSIZE];

    // Concatenate strings to get method = "DELETE filename"
    sprintf(method, "DELETE %s", file);

    // Send to the server
    SSL_write(ssl, method, sizeof(method));
    debug_printf("[%s method sent]\n", method);
};

void delete_local_file(char *file) {
    int file_i, found_file = 0;
    char filepath[MAXNAME];
    struct tailq_entry *ignoredfile_node;
    sprintf(filepath, "%s/%s", user_sync_dir_path, file);

    pthread_mutex_lock(&fileOperationMutex);
    pthread_mutex_lock(&inotifyMutex); // prevent inotify of deleting file before it's inserted in ignored file list

    // Search for file in user's list of files
    for(file_i = 0; file_i < MAXFILES; file_i++) {
        // Stop if it is found
        debug_printf("client_files[file_i].name: %s\n", client_files[file_i].name);
        if(strcmp(client_files[file_i].name, file) == 0) {
            found_file = 1;
            break;
        }
    }

    if(found_file) {
        // Set it's area free
        strcpy(client_files[file_i].name, "filename.ext");
        client_files[file_i].last_modified = time(NULL);
        client_files[file_i].size = FREE_FILE_SIZE;

        // Erase file
        if(remove(filepath) == 0) {
            // Add to ignore list, so inotify doesn't send a DELETE method again to server
            ignoredfile_node = malloc(sizeof(*ignoredfile_node));
            strcpy(ignoredfile_node->filename, file);
            if (ignoredfile_node == NULL) {
                perror("malloc failed");
            }

            TAILQ_INSERT_TAIL(&ignoredfiles_tailq_head, ignoredfile_node, entries);
            debug_printf("[Inseriu arquivo %s na lista pois foi apagado primeiramente em outro dispositivo]\n", file);
        } else {
            printf("Unable to delete file\n");
        }

    } else {
        printf("File not found\n");
    }

    pthread_mutex_unlock(&inotifyMutex);
    pthread_mutex_unlock(&fileOperationMutex);
};

void cmdList() {
    int valread;
    uint32_t nLeft;
    char buffer[MSGSIZE] = {0};

    SSL_write(ssl, "LIST", MSGSIZE);

    // Receive length
    SSL_read(ssl, buffer, MSGSIZE);
    nLeft = atoi(buffer); // TODO nao usar atoi?

    // Receive list of files in chunks
    while (nLeft > 0 && (valread = SSL_read(ssl, buffer, sizeof(buffer))) > 0) {
        buffer[valread] = '\0';
        printf("%s", buffer);
        nLeft -= strlen(buffer);
    }
};

void cmdMan() {
    printf("\nAvailable commands:\n");
    printf("get_sync_dir\n");
    printf("list\n");
    printf("download <filename.ext>\n");
    printf("upload <path/filename.ext>\n");
    printf("delete <filename.ext>\t\tDelete file from server\n");
    printf("help\n");
    printf("exit\n\n");
};

void cmdExit() {
    close_connection();
}

void sync_client() {
    char buffer[MSGSIZE];

    pthread_mutex_lock(&fileOperationMutex);
    debug_printf("[Syncing...]\n");

    SSL_write(ssl, "SYNC", MSGSIZE);

    // Receive number of files
    SSL_read(ssl, buffer, MSGSIZE);
    sync_files_left = atoi(buffer); // TODO nao usar atoi? atol? outras funcoes mais seguras?
    original_sync_files_left = sync_files_left;

    debug_printf("[Syncing done]\n");
    pthread_mutex_unlock(&fileOperationMutex);
}

void* sync_daemon(void* unused) {
    int length, fd, wd;
    char buffer[BUF_LEN];
    struct tailq_entry *ignoredfile_node;
    struct tailq_entry *tmp_ignoredfile_node;

    fd = inotify_init(); // Blocking by default

    if ( fd < 0 ) {
        perror( "inotify_init" );
    }

    wd = inotify_add_watch( fd, user_sync_dir_path,
                            IN_CLOSE_WRITE | IN_MOVE | IN_DELETE );

    while (1) {
        int i = 0;
        char filepath[MAXNAME];

        length = read( fd, buffer, BUF_LEN );

        if ( length < 0 ) {
            perror( "read" );
        }

        while ( i < length ) {
            struct inotify_event *event = ( struct inotify_event * ) &buffer[ i ];
            if ( event->len ) {
                if ( !(event->mask & IN_ISDIR) && event->name[0] != '.' ) { // If it's a file and it's not hidden
                    pthread_mutex_lock(&inotifyMutex); // prevent inotify of getting file before it's inserted in ignored file list

                    // Search for file in list of ignored files
                    for (ignoredfile_node = TAILQ_FIRST(&ignoredfiles_tailq_head); ignoredfile_node != NULL; ignoredfile_node = tmp_ignoredfile_node) {
                        if (strcmp(ignoredfile_node->filename, event->name) == 0) {
                            debug_printf("[Daemon: Found file %s in ignored files list]\n", event->name);
                            break;
                        }
                        tmp_ignoredfile_node = TAILQ_NEXT(ignoredfile_node, entries);
                    }

                    // If it's in the list, remove it
                    if(ignoredfile_node != NULL) {
                        TAILQ_REMOVE(&ignoredfiles_tailq_head, ignoredfile_node, entries);
                        pthread_mutex_unlock(&inotifyMutex);
                    } else {
                        // It's not in the list, so it was modified locally and changes need to be propagated to server
                        pthread_mutex_unlock(&inotifyMutex);
                        if ( (event->mask & IN_CLOSE_WRITE) || (event->mask & IN_MOVED_TO)  ) {
                            debug_printf( "[Daemon: The file %s was created, modified, or moved from somewhere.]\n", event->name );
                            sprintf(filepath, "%s/%s", user_sync_dir_path, event->name);
                            usleep(50000); // prevent inotify of getting file before it's metadata is saved by file manager
                            send_file(filepath);
                        } else if ( event->mask & IN_DELETE  ) {
                            debug_printf( "[Daemon: The file %s was deleted.]\n", event->name );
                            delete_server_file(event->name);
                        } else if ( event->mask & IN_MOVED_FROM  ) {
                            debug_printf( "[Daemon: The file %s was renamed.]\n", event->name );
                            sprintf(filepath, "%s/%s", user_sync_dir_path, event->name);
                            delete_server_file(event->name);
                        }
                    }
                }
            }
            i += EVENT_SIZE + event->len;
        }
    }
    ( void ) inotify_rm_watch( fd, wd );
    ( void ) close( fd );

    exit( 0 );
}

void cmdGetSyncDir() {
    pthread_t thread_id;
    int n, i;
    char file_path[256];
    struct dirent **namelist;
    struct stat st;

    // Create user sync_dir if it doesn't exist and then sync files
    if(makedir_if_not_exists(user_sync_dir_path) == 0){
        // Create inotify thread for monitoring file changes
        if (pthread_create(&thread_id, NULL, sync_daemon, NULL) < 0) {
            perror("could not create inotify thread");
        }
        inotify_run = 1;
        sync_client();
        if (original_sync_files_left > 0) {
            pthread_barrier_wait(&syncbarrier); // wait for syncing all files
        }
    } else if(inotify_run == 0){
        // Dir exists // REVIEW pode entrar aqui se a funcao makedir_if_not_exists der erro tb...
        // Insert data into struct file_info
        n = scandir(user_sync_dir_path, &namelist, 0, alphasort);
        if (n > 2) { // Starting in i=2, it doesn't show '.' and '..'
            for (i = 2; i < n; i++) {
                sprintf(file_path, "%s/%s", user_sync_dir_path, namelist[i]->d_name);
                stat(file_path, &st);

                strcpy(client_files[i-2].name, namelist[i]->d_name);
                client_files[i-2].last_modified = st.st_mtime;
                client_files[i-2].size = st.st_size;

                free(namelist[i]);
            }
        }
        free(namelist);

        // Create inotify thread for monitoring file changes
		if (pthread_create(&thread_id, NULL, sync_daemon, NULL) < 0) {
		    perror("could not create inotify thread");
		}
		inotify_run = 1;
    }
};

// Listen to server calls (PUSH, DELETE, ...)
void* server_listener(void* unused) {
    int read_size;
    uint16_t client_server_port;
    char buffer[MSGSIZE];

    // Receive server port for new socket
    SSL_read(ssl, buffer, MSGSIZE);
    client_server_port = atoi(buffer); // TODO nao usar atoi? atol? outras funcoes mais seguras?
    debug_printf("client_server_port: %d\n", client_server_port);

    // Connect and attach SSL
    ssl_cls = connect_server(server_host, client_server_port);
    if (ssl_cls == NULL) {
        return NULL;
    }

    pthread_barrier_wait(&serverlistenerbarrier);

    // Receive a message from server
    while ((read_size = SSL_read(ssl_cls, buffer, MSGSIZE)) > 0 ) {
        // end of string marker
        buffer[read_size] = '\0';

        if (!strncmp(buffer, "PUSH", 4)) {
            debug_printf("[received PUSH from server]\n");
            get_file(buffer + 5, user_sync_dir_path);
            if (sync_files_left > 1){
                sync_files_left--;
            } else if(sync_files_left == 1  && original_sync_files_left >= 1){
                pthread_barrier_wait(&syncbarrier);
                sync_files_left--;
            }
        } else if (!strncmp(buffer, "DELETE", 6)) {
            debug_printf("[received DELETE from server]\n");
            delete_local_file(buffer + 7);
        }
    }

    if (read_size == 0) {
        printf("Server disconnected. Closing connection...");
        close_connection();
        exit(0);
    } else if (read_size == -1) {
        perror("recv failed");
    }

    exit(0);
}

void copy_file(char *file1, char *file2){
    char buffer[BUFSIZ];
    size_t n;
    struct stat st;
    FILE *f1, *f2;
    struct utimbuf new_times;

    if (stat(file2, &st) == 0) {
        printf("File already exists\n");
    } else {
        f1 = fopen (file1, "r");
        f2 = fopen (file2, "wb");

        if (f1 != NULL && f2 != NULL) {
            while ((n = fread(buffer, sizeof(char), sizeof(buffer), f1)) > 0){
                if (fwrite(buffer, sizeof(char), n, f2) != n)
                    printf("Copy failed\n");
            }
            fclose (f1);
            fclose (f2);
        }

        new_times.modtime = st.st_mtime;
        new_times.actime = time(NULL);
        utime(file2, &new_times);
    }
}

void ShowCerts(SSL* ssl)
{   X509 *cert;
    char *line;

    cert = SSL_get_peer_certificate(ssl); /* get the server's certificate */
    if ( cert != NULL )
    {
        printf("Server certificates:\n");
        line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
        printf("Subject: %s\n", line);
        free(line);       /* free the malloc'ed string */
        line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
        printf("Issuer: %s\n", line);
        free(line);       /* free the malloc'ed string */
        X509_free(cert);     /* free the malloc'ed certificate copy */
    }
    else
        printf("Info: No client certificates configured.\n");
}

int main(int argc, char * argv[]) {
    char cmd[256];
    char filename[256];
    char buffer[MSGSIZE];
    char * token;
    int valread, i;
    pthread_t thread_id;

    // Check number of parameters
    if (argc < 4) {
        printf("Usage: %s <user> <IP> <port>\n", argv[0]);
        return 1;
    }

    // Initialize SSL
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    // Connect to server
    debug_printf("[Client started with parameters User=%s IP=%s Port=%s]\n", argv[1], argv[2], argv[3]);
    strcpy(server_host, argv[2]);
    strcpy(server_user, argv[1]);
    server_port = atoi(argv[3]);
    ssl = connect_server(server_host, server_port);
    if (ssl == NULL) {
        return -1;
    }
    //ShowCerts(ssl);

    // Initialize the tail queue
    TAILQ_INIT(&ignoredfiles_tailq_head);

    // Set size to FREE_FILE_SIZE for all the files, so we can check if a slot is free later
    for (i = 0; i < MAXFILES; i++) {
        client_files[i].size = FREE_FILE_SIZE;
    }

    // Define path to user sync_dir folder
    sprintf(user_sync_dir_path, "%s/sync_dir_%s", getenv("HOME"), server_user);

    pthread_barrier_init(&syncbarrier, NULL, 2);
    pthread_barrier_init(&serverlistenerbarrier, NULL, 2);

    // Send username to server
    SSL_write(ssl, server_user, sizeof(server_user));
    // Detect if connection was closed
    valread = SSL_read(ssl, buffer, MSGSIZE);
    if (valread == 0 || strncmp(buffer, TRANSMISSION_CONFIRM, TRANSMISSION_MSG_SIZE) != 0) {
        printf("%s is already connected in two devices. Closing connection...\n", server_user);
        return 0;
    }

    debug_printf("[Connection established]\n");

    // Create thread for server_listener
    if (pthread_create(&thread_id, NULL, server_listener, NULL) < 0) {
        perror("could not create server_listener thread");
        return 1;
    }

    // Create user sync_dir and sync files
    printf("Syncing...");
    pthread_barrier_wait(&serverlistenerbarrier); // wait for server_listener connection

    cmdGetSyncDir();

    printf("Done.\n\n");

    printf("Horário Lógico: %d\n", getLogicalTime());

    printf("Welcome to Dropbox! - v 2.0\n");
    cmdMan();

    // Handle user input
    while (1) {
        printf("Dropbox> ");
        // TODO Ajustar se usuário tecla enter sem inserir nada antes
        scanf("%s", cmd);
        if ((token = strtok(cmd, " \t")) != NULL) {
            if (strcmp(token, "exit") == 0) {
                cmdExit();
                break;
            } else if (strcmp(token, "upload") == 0) {
                scanf("%s", filename);
                // Copy file to user sync_dir.
                char newfilepath[MAXNAME];
                sprintf(newfilepath, "%s/%s", user_sync_dir_path, basename(filename));
                copy_file(filename, newfilepath);
                // send_file(filename); Not necessary anymore because inotify gets it and upload
            } else if (strcmp(token, "download") == 0) {
                scanf("%s", filename);
                char cwd[256];
                getcwd(cwd, sizeof(cwd));
                get_file(filename, cwd);
            } else if (strcmp(token, "delete") == 0) {
                scanf("%s", filename);
                delete_server_file(filename);
            } else if (strcmp(token, "list") == 0) cmdList();
            else if (strcmp(token, "get_sync_dir") == 0) cmdGetSyncDir();
            else if (strcmp(token, "help") == 0) cmdMan();
            else printf("Invalid command! Type 'help' to see the available commands\n");
        }
    }

    return 0;
}

void close_connection() {
    // Stop both reception and transmission.
    shutdown(sock, 2);
    SSL_free(ssl);
}

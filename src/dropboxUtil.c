#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "../include/dropboxUtil.h"

pthread_mutex_t fileMutex = PTHREAD_MUTEX_INITIALIZER;

// Debug messages, displayed only when DEBUG=1
void debug_printf(const char* message, ...) {
    if(DEBUG) {
        va_list args;
        va_start(args, message);
        vprintf(message, args);
        va_end(args);
    }
}

/* Create dir if it doesn't exist
   Return:   0: created dir
            -1: dir exists or there was an error when trying to create it
 */
int makedir_if_not_exists(const char* path) {
    struct stat st = {0};
    int result = -1;
    pthread_mutex_lock(&fileMutex);
    if (stat(path, &st) == -1) {
        result = mkdir(path, 0700);
    }
    pthread_mutex_unlock(&fileMutex);
    return result;
}

// If file exists and is a file (not a directory)
int file_exists(const char* path) {
    struct stat st = {0};
    pthread_mutex_lock(&fileMutex);
    int stat_result = (stat(path, &st) == 0) && S_ISREG(st.st_mode);
    pthread_mutex_unlock(&fileMutex);
    return stat_result;
}

SSL* start_server(char * host, int port) {


}

SSL* connect_server(char * host, int port) {
    int sock = 0;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    SSL_CTX *ctx;
    const SSL_METHOD *method;
    SSL *ssl;

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    method = TLSv1_2_client_method();
    ctx = SSL_CTX_new(method);
    if (ctx == NULL){
      ERR_print_errors_fp(stderr);
      abort();
    }

    server = gethostbyname(host);
    if (server == NULL) {
        printf("ERROR, no such host\n");
        return NULL;
    }

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return NULL;
    }

    memset(&serv_addr, '0', sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr = *((struct in_addr *) server->h_addr);
    bzero(&(serv_addr.sin_zero), 8);

    if (connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed. \n");
        return NULL;
    }

    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);
    if (SSL_connect(ssl) == -1){
        printf("SSL connection refused\n");
        ERR_print_errors_fp(stderr);
        return(NULL);
    }

    SSL_CTX_free(ctx); // release context

    return ssl;
}

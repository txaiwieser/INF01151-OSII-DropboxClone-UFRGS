#define DEBUG 1 // Enable (1) or disable (0) debug messages
#define MAXNAME 255 // Maximum filename length
#define MAXFILES 200 // Maximum number of files in user dir
#define METHODSIZE 255 // Method messages (DOWNLOAD filename, UPLOAD filename, PUSH filename, etc) length
#define FREE_FILE_SIZE -1

// Both constants must have TRANSMISSION_MSG_SIZE characteres!
#define TRANSMISSION_CONFIRM "OK"
#define TRANSMISSION_CANCEL "ER"
#define TRANSMISSION_MSG_SIZE 2

#define MIN(a,b) (a < b)? a : b

typedef struct file_info {
  char name[MAXNAME]; // refere-se ao nome do arquivo, incluindo a extensão
  time_t last_modified; // refere-se a data da última modificação no arquivo
  int size; // indica o tamanho do arquivo, em bytes
} FILE_INFO_t;

int connect_server(char *host, int port);
void debug_printf(const char* message, ...);
int makedir_if_not_exists(const char* path);
int dir_exists(const char* path);
int file_exists(const char* path);

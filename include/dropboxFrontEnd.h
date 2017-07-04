#include "dropboxUtil.h"

#define MAXCLIENTS 20
#define FREE_CLIENT_SLOT_USERID "available"

typedef struct frontend_client {
  SSL *devices[MAXDEVICES]; // associado aos dispositivos do usuário
  SSL *devices_server[MAXDEVICES]; // socket 'servidor' do cliente, que recebe requisições de PUSH e DELETE
  char userid[MAXNAME]; // id do usuário no servidor, que deverá ser único. Informado pela linha de comando.
} FRONTEND_CLIENT_t;

void *client_server_handler(void *socket_desc);
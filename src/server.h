#ifndef SERVER_H
#define SERVER_H 1

#include "types.h"

int  start_server        (void);
int  server_accept       (void);
int  server_read         (void);
void stop_server         (void);
void debug_print_clients (void);

#endif /* ! SERVER_H */

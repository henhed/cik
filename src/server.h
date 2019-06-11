#ifndef SERVER_H
#define SERVER_H 1

#include "types.h"

int  start_server        (void);
void stop_server         (void);
void load_request_log    (int);
void debug_print_clients (int);

#endif /* ! SERVER_H */

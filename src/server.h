#ifndef SERVER_H
#define SERVER_H 1

#include "types.h"

int  start_server  (void);
int  server_accept (void);
void server_read   (void);
void stop_server   (void);

#endif /* ! SERVER_H */

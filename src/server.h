#ifndef SERVER_H
#define SERVER_H 1

#include "types.h"

int  start_server        (void);
void stop_server         (void);

StatusCode read_request           (Client *, Request *);
StatusCode read_request_payload   (Client *, u8 *, u32);
StatusCode write_response         (Client *, Response *);
StatusCode write_response_payload (Client *, u8 *, u32);
void       close_client           (Client *);
void       flush_worker_logs      (void);

void load_request_log    (int);
void debug_print_clients (int);
void debug_print_workers (int);

#endif /* ! SERVER_H */

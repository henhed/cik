#ifndef CONTROLLER_H
#define CONTROLLER_H 1

#include "types.h"

int         init_controller (void);
StatusCode  handle_request  (Client *, Request *, Payload **);

#endif /* ! CONTROLLER_H */

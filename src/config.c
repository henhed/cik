#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "types.h"

char log_filename[0x400] = "/tmp/cik-server.log";

static RuntimeConfig runtime_config = {
  .listen_address = INADDR_ANY,
  .listen_port    = 5555,
  .log_filename   = log_filename
};

void parse_variable (const char *, int, const char *, char *);

RuntimeConfig *
parse_args (int argc, char **argv)
{
  FILE *cfg_file;
  char *filename;
  char *line;
  char  buffer[0x400];
  int   lineno = 0;

  cik_assert (argc > 0);
  if (argc == 1)
    return &runtime_config;

  filename = argv[1];
  cfg_file = fopen (filename, "r");
  if (!cfg_file)
    {
      err_print ("Could not open %s: %s\n", filename, strerror (errno));
      return NULL;
    }

  while ((line = fgets(buffer, sizeof (buffer), cfg_file)))
    {
      char *c, *name, *value;

      ++lineno;

      c = line;

      while (isspace (*c))
        ++c;

      if (*c == '#')
        continue;

      name = c;

      while (!isspace (*c) && !(*c == '=') && !(*c == '\0'))
        ++c;

      if (*c == '\0')
        continue;

      *c++ = '\0';

      while (isspace (*c))
        ++c;

      if (*c == '=')
        ++c;
      else
        {
          err_print ("Missing assignment in %s on line %d\n", filename, lineno);
          continue;
        }

      while (isspace (*c))
        ++c;

      if (*c == '\0')
        {
          err_print ("Empty value in %s on line %d\n", filename, lineno);
          continue;
        }

      value = c;

      while (*c != '\0')
        ++c;
      while (isspace (*(c - 1)))
        --c;
      *c = '\0';

      parse_variable (filename, lineno, name, value);
    }

  return &runtime_config;
}

void
parse_variable (const char *filename, int lineno, const char *name, char *value)
{
  if (0 == strcmp(name, "listen_address"))
    {
      struct addrinfo  hints = { 0 };
      struct addrinfo *result, *rp;
      int err;
      bool found;

      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_flags = 0;
      hints.ai_protocol = IPPROTO_TCP;

      err = getaddrinfo (value, NULL, &hints, &result);
      if (err != 0)
        {
          err_print ("Failed to parse address: %s\n", gai_strerror (err));
          return;
        }

      found = false;
      for (rp = result; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET
            && rp->ai_socktype == SOCK_STREAM
            && rp->ai_protocol == IPPROTO_TCP
            && rp->ai_addrlen == sizeof (sockaddr_in_t))
          {
            sockaddr_in_t *addr = (sockaddr_in_t *) rp->ai_addr;
            runtime_config.listen_address = addr->sin_addr.s_addr;
            found = true;
            break;
          }
      }

      freeaddrinfo (result);

      if (!found)
        {
          err_print ("Failed to lookup address: %s\n", gai_strerror (err));
          return;
        }
    }
  else if (0 == strcmp(name, "listen_port"))
    {
      char *endptr = NULL;
      long int port = strtol (value, &endptr, 10);
      if (endptr == value)
        {
          err_print ("Could not parse port number in %s on line %d\n",
                     filename, lineno);
          return;
        }

      if (port < 0 || port > 0xFFFF)
        {
          err_print ("Port number %ld out of range in %s on line %d\n",
                     port, filename, lineno);
          return;
        }

      runtime_config.listen_port = port;
    }
  else if (0 == strcmp(name, "log_filename"))
    {
      strncpy (log_filename, value, sizeof (log_filename));
      log_filename[sizeof (log_filename) - 1] = '\0';
    }
  else
    {
      err_print ("Unknown variable '%s' in %s on line %d\n",
                 name, filename, lineno);
    }
}

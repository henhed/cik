#ifndef PRINT_H
#define PRINT_H 1

#include <stdio.h>

#include "config.h"

#define BLUE(string)   "\e[0;34m" string "\e[0m"
#define GREEN(string)  "\e[0;32m" string "\e[0m"
#define RED(string)    "\e[1;31m" string "\e[0m"
#define YELLOW(string) "\e[1;33m" string "\e[0m"

#define LINEWIDTH 80
#define HLINESTR "----------------------------------------" \
                 "----------------------------------------"
#define BLANKSTR "                                        " \
                 "                                        "

#if DEBUG
# define dbg_print printf
#else
# define dbg_print(fmt, ...)
#endif

#define wrn_print(fmt, ...)                                     \
  fprintf (stderr, YELLOW ("W") " %s:%d: %s: " fmt,             \
           __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define err_print(fmt, ...)                                     \
  fprintf (stderr, RED ("E") " %s:%d: %s: " fmt,                \
           __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#endif /* ! PRINT_H */

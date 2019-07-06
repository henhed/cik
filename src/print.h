#ifndef PRINT_H
#define PRINT_H 1

#include <stdio.h>

#include "config.h"

#define BEGIN_BLUE     "\e[0;34m"
#define BEGIN_GREEN    "\e[0;32m"
#define BEGIN_RED      "\e[1;31m"
#define BEGIN_YELLOW   "\e[1;33m"
#define BEGIN_GRAY     "\e[38;5;244m"
#define RESET_ANSI_FMT "\e[0m"

#define BLUE(string)   BEGIN_BLUE   string RESET_ANSI_FMT
#define GREEN(string)  BEGIN_GREEN  string RESET_ANSI_FMT
#define RED(string)    BEGIN_RED    string RESET_ANSI_FMT
#define YELLOW(string) BEGIN_YELLOW string RESET_ANSI_FMT
#define GRAY(string)   BEGIN_GRAY   string RESET_ANSI_FMT

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

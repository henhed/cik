#ifndef PRINT_H
#define PRINT_H 1

#include <stdio.h>

#define BLUE(string)   "\e[0;34m" string "\e[0m"
#define GREEN(string)  "\e[0;32m" string "\e[0m"
#define RED(string)    "\e[1;31m" string "\e[0m"
#define YELLOW(string) "\e[1;33m" string "\e[0m"

#define LINEWIDTH 80
#define HLINESTR "----------------------------------------" \
                 "----------------------------------------"
#define BLANKSTR "                                        " \
                 "                                        "
#endif /* ! PRINT_H */

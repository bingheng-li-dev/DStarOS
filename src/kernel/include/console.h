#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#include <stdbool.h>

void consoleInit(void);
void printf(char *fmt, ...);
void panic(char *s);

#endif
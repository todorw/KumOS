#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define KEY_UP    ((char)0xC8)
#define KEY_DOWN  ((char)0xC9)
#define KEY_LEFT  ((char)0xCA)
#define KEY_RIGHT ((char)0xCB)
#define KEY_HOME  ((char)0xCC)
#define KEY_END   ((char)0xCD)
#define KEY_DEL   ((char)0xCE)

void keyboard_init(void);
char keyboard_getchar(void);
char keyboard_getchar_blocking(void);
int  keyboard_getline(char *buf, int maxlen);
int  keyboard_ctrl_held(void);
int  keyboard_alt_held(void);

int keyboard_has_input(void);

int keyboard_check_ctrlc(void);

int keyboard_check_ctrlz(void);

int keyboard_check_alttab(void);

void keyboard_inject(const char *s);

#endif

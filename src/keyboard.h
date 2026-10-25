#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* Special (non-ASCII) key codes returned by keyboard_getchar()/blocking()
   for extended PS/2 scancodes. Cast so the bit pattern survives char's
   (signed on this target) promotion to int consistently on both the
   write side (process_scancode) and read side (any comparison). */
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

/* Scans pending input for a bare Ctrl+C (ASCII 3/ETX) and consumes
   everything up to and including it if found. Used by sched_waitpid()
   to let Ctrl+C interrupt a blocked wait - see there for why this only
   catches the byte when the waited-on child isn't itself mid-read. */
int keyboard_check_ctrlc(void);

/* Same as keyboard_check_ctrlc() but for Ctrl+Z (ASCII 26/SUB) - used by
   sched_waitstatus() to let Ctrl+Z stop a foreground job instead of
   killing it. */
int keyboard_check_ctrlz(void);

/* Same peek-and-requeue-if-not-mine approach, for Alt+Tab specifically -
   the GUI's window-switch hotkey needs to reach past a focused window
   that's otherwise consuming every keystroke itself (a live real kush
   session hosted in a terminal window, see gui.c's boot_term_pid), same
   as Ctrl+C/Ctrl+Z already reach past a foreground ring-3 program. Only
   matches while Alt is actually held, so a plain Tab (kush's own
   completion key) is never touched. */
int keyboard_check_alttab(void);

void keyboard_inject(const char *s);

#endif

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

typedef struct {
    int32_t  x, y;
    int32_t  dx, dy;
    uint8_t  buttons;
    uint8_t  pressed;
    int      left, right, middle;
} mouse_state_t;

void           mouse_init(void);
mouse_state_t *mouse_get(void);
int            mouse_ready(void);
uint8_t        mouse_take_pressed(void);

void mouse_set_bounds(int32_t max_x, int32_t max_y);

#endif

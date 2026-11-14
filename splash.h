#ifndef AXIOME_SPLASH_H
#define AXIOME_SPLASH_H

#include <stdint.h>

void splash_show(void);

void splash_set_progress(uint32_t permille);

void splash_note_log(void);

void splash_hide(void);

#endif /* AXIOME_SPLASH_H */

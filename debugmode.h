#ifndef AXIOME_DEBUGMODE_H
#define AXIOME_DEBUGMODE_H

/* Kernel debug mode (axboot.cfg `debug=1`).
 *
 * When enabled, printk() mirrors kernel log output to the framebuffer.
 * When disabled (the default), printk() only reaches the serial console and
 * the /var/log/kernel.log ring; the graphical boot splash stays clean.
 * Panics always force framebuffer output. */
void kernel_debug_init(const char *cmdline);
int  kernel_debug_mode(void);
int  kernel_debug_fb(void);
void kernel_debug_force_fb(int on);

/* `splash=0` / `nosplash` in the command line disables the graphical splash
   even when debug mode is off. */
int  kernel_splash_enabled(void);

#endif /* AXIOME_DEBUGMODE_H */

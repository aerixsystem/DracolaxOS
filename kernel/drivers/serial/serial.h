/* kernel/serial.h — COM1 serial port (debug output) */
#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);
void serial_putchar(char c);
void serial_print(const char *s);

#endif /* SERIAL_H */

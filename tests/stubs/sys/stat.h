#pragma once
// newlib-style POSIX bits for the PC syntax check
struct stat { unsigned int st_mode; long st_size; };
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#define S_ISREG(m) (((m) & 0170000) == 0100000)
int mkdir(const char *path, unsigned int mode);
int stat(const char *path, struct stat *st);

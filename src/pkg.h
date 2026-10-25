#ifndef PKG_H
#define PKG_H
#include <stdint.h>

int pkg_count(void);
const char *pkg_name_at(int i);
const char *pkg_fname_at(int i);
const char *pkg_desc_at(int i);
int pkg_is_installed(int i);

int pkg_install(const char *name);
int pkg_remove(const char *name);

int pkg_find_index(const char *name);
int pkg_blob_at(int i, const uint8_t **start, const uint8_t **end);
int pkg_blob_by_fname(const char *fname, const uint8_t **start, const uint8_t **end);

#endif

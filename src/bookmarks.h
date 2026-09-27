#ifndef BOOKMARKS_H
#define BOOKMARKS_H

#define BOOKMARK_MAX 16
#define NAME_MAX     24
#define HOST_MAX     40

/* A site. The drive transfers use is not part of a site any more: it is
 * chosen with F7 and applies to every connection. Older config files with
 * a sixth field still load; the field is ignored. */
typedef struct {
  char name[NAME_MAX];
  char host[HOST_MAX];
  unsigned int port;
  unsigned char emul;       /* 0: EMUL_PETSCII, 1: EMUL_ANSI */
  unsigned char res;        /* 0: RES_40X25, 1: RES_80X25, 2: RES_80X50 */
} bookmark_t;

void bookmarks_init(unsigned char boot_drive);
unsigned char bookmarks_count(void);
bookmark_t *bookmarks_get(unsigned char index);
unsigned char bookmarks_add(const bookmark_t *bm);
unsigned char bookmarks_update(unsigned char index, const bookmark_t *bm);
unsigned char bookmarks_delete(unsigned char index);
unsigned char bookmarks_save(unsigned char drive);
unsigned char bookmarks_load(unsigned char drive);

#endif

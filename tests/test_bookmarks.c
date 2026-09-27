#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "bookmarks.h"

int main(void)
{
  bookmark_t bm;
  bookmark_t *p;

  printf("Testing bookmarks...\n");

  /* Reset */
  while (bookmarks_count() > 0) {
    bookmarks_delete(0);
  }
  assert(bookmarks_count() == 0);

  /* Add bookmark */
  strcpy(bm.name, "Test BBS");
  strcpy(bm.host, "test.bbs.org");
  bm.port = 23;
  bm.emul = 1; /* ANSI */
  bm.res = 1;  /* 80x25 */
  assert(bookmarks_add(&bm));
  assert(bookmarks_count() == 1);

  p = bookmarks_get(0);
  assert(p != 0);
  assert(strcmp(p->name, "Test BBS") == 0);
  assert(strcmp(p->host, "test.bbs.org") == 0);
  assert(p->port == 23);
  assert(p->emul == 1);
  assert(p->res == 1);

  /* Update bookmark */
  strcpy(bm.name, "Renamed BBS");
  bm.port = 2323;
  assert(bookmarks_update(0, &bm));
  p = bookmarks_get(0);
  assert(strcmp(p->name, "Renamed BBS") == 0);
  assert(p->port == 2323);

  /* Delete bookmark */
  assert(bookmarks_delete(0));
  assert(bookmarks_count() == 0);

  /* Save and load to file */
  strcpy(bm.name, "Saved BBS");
  strcpy(bm.host, "saved.org");
  bm.port = 6400;
  bm.emul = 0;
  bm.res = 0;
  bookmarks_add(&bm);
  assert(bookmarks_save(0));

  /* Clear in-memory and reload */
  bookmarks_delete(0);
  assert(bookmarks_count() == 0);
  assert(bookmarks_load(0));
  assert(bookmarks_count() == 1);
  p = bookmarks_get(0);
  assert(strcmp(p->name, "Saved BBS") == 0);
  assert(p->port == 6400);

  /* A line longer than 50 characters, and a full table, survive a round
   * trip: on the MEGA65 the parser's scratch once overlapped the line
   * buffer past that point, and the table now lives in far memory. */
  {
    unsigned char i;
    bookmarks_delete(0);
    for (i = 0; i < BOOKMARK_MAX; i++) {
      snprintf(bm.name, sizeof(bm.name), "Site %u", i);
      snprintf(bm.host, sizeof(bm.host), "a-rather-long-host-name-%u.example.org", i);
      bm.port = (unsigned int)(1000 + i);
      bm.emul = i & 1;
      bm.res = i % 3;
          assert(bookmarks_add(&bm));
    }
    assert(!bookmarks_add(&bm));               /* full */
    assert(bookmarks_count() == BOOKMARK_MAX);
    assert(bookmarks_save(0));
    while (bookmarks_count() > 0) bookmarks_delete(0);
    assert(bookmarks_load(0));
    assert(bookmarks_count() == BOOKMARK_MAX);
    for (i = 0; i < BOOKMARK_MAX; i++) {
      char want[HOST_MAX];
      snprintf(want, sizeof(want), "a-rather-long-host-name-%u.example.org", i);
      p = bookmarks_get(i);
      assert(p && strcmp(p->host, want) == 0);
      assert(p->port == 1000 + i && p->emul == (i & 1) && p->res == i % 3);
    }
    /* Deleting from the middle closes the gap; deleting the last is a
     * zero-length move. */
    assert(bookmarks_delete(5));
    p = bookmarks_get(5);
    assert(p && strcmp(p->name, "Site 6") == 0);
    assert(bookmarks_delete(bookmarks_count() - 1));
    assert(bookmarks_count() == BOOKMARK_MAX - 2);
  }

  printf("Bookmarks tests passed successfully!\n");
  return 0;
}

#include <string.h>
#include "platform/m65_screen.h"
#include "platform/m65_font.h"
#include "platform/m65_boot.h"
#include "platform/m65_cbmdos.h"
#include "platform/m65_exit.h"
#include "ansi.h"
#include "petscii.h"
#include "telnet.h"
#include "bookmarks.h"
#include "zmodem.h"
#include "xmodem.h"
#include "ui.h"
#include "netutil.h"

#ifdef __MEGA65__
#include "mega65/memory.h"
#endif

static unsigned char active_emul = EMUL_PETSCII;
static unsigned char active_res = RES_80X25;
static unsigned char active_speed = SPEED_MAX;
static const char *active_bbs_name = "Connected";

static void telnet_out_dispatch(unsigned char c)
{
  if (active_emul == EMUL_ANSI) {
    ansi_putc(c);
  } else {
    petscii_putc(c);
  }
}

static void telnet_send_dispatch(const unsigned char *data, unsigned int len)
{
  net_send(data, len);
}

static void zmodem_progress_display(const zmodem_status_t *st)
{
  ui_draw_zmodem_progress(st);
}

static void xmodem_progress_display(const xmodem_status_t *st)
{
  ui_draw_xmodem_progress(st);
}

static char transfer_fname[20];
static unsigned long transfer_fsize;

/* A line on the bottom row for a moment. */
static __attribute__((noinline)) void show_briefly(const char *msg, unsigned char color, unsigned char frames)
{
  unsigned char row = (unsigned char)(m65_screen_rows() - 1), cols = m65_screen_cols();
  unsigned char len = (unsigned char)strlen(msg);
  unsigned int spins = 0;
  unsigned char wraps = 0;
  m65_screen_clear_row(row, ' ', 1);
  m65_screen_puts((unsigned char)((cols - len) / 2), row, msg, color);
  net_frames = 0;
  while (net_frames < frames) {
    net_poll();
    if (++spins == 0 && ++wraps >= 20) break;
  }
}

/* One line on the bottom row saying how the transfer ended, held for a
 * moment; the codes are the same for both protocols. */
static __attribute__((noinline)) void report_transfer(unsigned char code)
{
  static const char *const why[] = { "complete", "disk error", "bad data", "cancelled", "timed out" };
  char msg[40];
  strcpy(msg, "Transfer ");
  strcat(msg, why[code > 4 ? 4 : code]);
  show_briefly(msg, code ? 2 : 1, 90);
}

/* The modem speed: characters that may be drawn per frame, in
 * sixteenths, ten bits to a character (start and stop bits included) at
 * 50 frames a second: 300 baud is 30 a second, 9600 is 960. */
static const unsigned int speed_per_frame16[4] = { 10, 38, 77, 307 };
static unsigned int meter_acc;
static unsigned char meter_last_frame;

/* Feeds up to `avail` bytes at the session's speed and returns how many
 * were taken; everything at once when the speed is unlimited. */
static __attribute__((noinline)) unsigned int meter_feed(const unsigned char *p, unsigned int avail)
{
  unsigned int k = avail;
  if (active_speed < SPEED_MAX) {
    unsigned char f = (unsigned char)PEEK(0xd7fa);
    if (f != meter_last_frame) {
      meter_last_frame = f;
      meter_acc += speed_per_frame16[active_speed];
    }
    k = meter_acc >> 4;
    if (k > avail) k = avail;
    meter_acc -= k << 4;
  }
  if (k) telnet_feed(p, k);
  return k;
}

static __attribute__((noinline)) void handle_zmodem_upload(void)
{
  if (ui_file_picker(work_drive, transfer_fname, &transfer_fsize) && transfer_fname[0]) {
    report_transfer(zmodem_send(transfer_fname, work_drive, transfer_fsize));
  }
}

static __attribute__((noinline)) void handle_xmodem_upload(void)
{
  if (ui_file_picker(work_drive, transfer_fname, &transfer_fsize) && transfer_fname[0]) {
    report_transfer(xmodem_send(transfer_fname, work_drive, transfer_fsize));
  }
}

static __attribute__((noinline)) void handle_upload(unsigned char act)
{
  if (act == TRANSFER_ACT_Z_UP) {
    handle_zmodem_upload();
  } else {
    handle_xmodem_upload();
  }
}

static __attribute__((noinline)) void handle_download(unsigned char act)
{
  if (act == TRANSFER_ACT_Z_DOWN) {
    report_transfer(zmodem_receive(work_drive));
  } else if (act == TRANSFER_ACT_X_DOWN) {
    unsigned char pr_row = (m65_screen_rows() > 25) ? 14 : 10;
    transfer_fname[0] = 0;
    if (ui_read_line(pr_row, "Save filename: ", transfer_fname, 16) && transfer_fname[0]) {
      report_transfer(xmodem_receive(transfer_fname, work_drive));
    }
  }
}

/* The screen for the active site: RES_40IN80 is an 80-column screen
 * with PETSCII drawn in a 40-column window in the middle, and the board
 * told it has 40 columns. */
static __attribute__((noinline)) void apply_res(void)
{
  unsigned char narrow = (active_res == RES_40IN80);
  m65_screen_set_res(narrow ? RES_80X25 : active_res);
  m65_screen_set_emul(active_emul);
  petscii_set_window(narrow ? 40 : 0, narrow ? 20 : 0);
}

static void term_flush(void)
{
  if (active_emul == EMUL_ANSI) ansi_flush();
  else petscii_flush();
}

static __attribute__((noinline)) void handle_terminal_transfer(void)
{
  unsigned char act = ui_transfer_menu();
  transfer_fname[0] = 0;
  transfer_fsize = 0;

  if (act == TRANSFER_ACT_Z_UP || act == TRANSFER_ACT_X_UP) {
    handle_upload(act);
  } else if (act == TRANSFER_ACT_Z_DOWN || act == TRANSFER_ACT_X_DOWN) {
    handle_download(act);
  }
  apply_res();
}

static __attribute__((noinline)) void send_terminal_key(unsigned char k)
{
  /* Transmit keystroke to BBS */
  if (active_emul == EMUL_ANSI) {
    if (k == KEY_UP) net_send((const unsigned char *)"\x1b[A", 3);
    else if (k == KEY_DOWN) net_send((const unsigned char *)"\x1b[B", 3);
    else if (k == KEY_RIGHT) net_send((const unsigned char *)"\x1b[C", 3);
    else if (k == KEY_LEFT) net_send((const unsigned char *)"\x1b[D", 3);
    else if (k == KEY_DEL) { unsigned char b = 0x08; net_send(&b, 1); }
    else net_send(&k, 1);
  } else {
    /* PETSCII direct with case mapping:
       ASCII 'a'-'z' (0x61-0x7a) -> PETSCII unshifted (0x41-0x5a)
       ASCII 'A'-'Z' (0x41-0x5a) -> PETSCII shifted (0xc1-0xda) */
    unsigned char out_k = k;
    if (k >= 'a' && k <= 'z') {
      out_k = (unsigned char)(k - 0x20);
    } else if (k >= 'A' && k <= 'Z') {
      out_k = (unsigned char)(k + 0x80);
    }
    net_send(&out_k, 1);
  }
}

static __attribute__((noinline)) void run_terminal_session(bookmark_t *bm)
{
#define RX_BUF_SIZE 128
#ifdef __MEGA65__
#define rx_buf ((unsigned char *)0x1d00)
#else
  static unsigned char rx_buf[RX_BUF_SIZE];
#endif
  unsigned int n = 0, pos = 0;     /* rx_buf holds n bytes, pos of them fed so far */

  active_bbs_name = bm->name;
  (void)active_bbs_name;
  active_emul = bm->emul;
  active_res = bm->res;
  active_speed = bm->speed;

  /* Configure screen and emulation */
  apply_res();
  m65_screen_cls();

  ansi_reset();
  petscii_reset();
  telnet_reset();
  telnet_set_terminal_type(active_emul == EMUL_ANSI ? "ANSI" : "PETSCII");
  {
    unsigned char c = (active_res == RES_40IN80) ? 40 : m65_screen_cols();
    telnet_set_window_size(c, m65_screen_rows());
    telnet_send_naws(c, m65_screen_rows());
  }
  telnet_send_init_negotiation();
  m65_screen_cursor_enable(1);

  for (;;) {
    unsigned char k;

    net_poll();

    /* Receive incoming data from socket, drawn at the session's speed */
    if (pos >= n) { n = net_recv(rx_buf, RX_BUF_SIZE); pos = 0; }
    if (pos < n) {
      pos += meter_feed(rx_buf + pos, n - pos);
      term_flush();

      /* Auto-detect ZModem download: what was received but not yet drawn
       * belongs to the transfer */
      if (telnet_check_zmodem()) {
        telnet_clear_zmodem();
        if (pos < n) telnet_pushback(rx_buf + pos, n - pos);
        pos = n;
        m65_screen_cursor_enable(0);
        report_transfer(zmodem_receive(work_drive));
        apply_res();
        m65_screen_cursor_enable(1);
      }
    } else {
      telnet_idle();
      term_flush();
      m65_screen_cursor_tick();
    }

    /* Check connection health */
    if (!net_alive()) {
      break;
    }

    /* Read user input */
    k = ui_key();
    if (k) {
      /* Function keys handling */
      if (IS_KEY_F1(k)) {
        /* Disconnect / return to Dialing Directory */
        break;
      } else if (IS_KEY_F3(k)) {
        if (pos < n) telnet_feed(rx_buf + pos, n - pos);   /* the board's text, before the transfer takes the stream */
        pos = n;
        m65_screen_cursor_enable(0);
        handle_terminal_transfer();
        m65_screen_cursor_enable(1);
      } else if (IS_KEY_F7(k)) {
        char msg[24];
        active_speed = (unsigned char)((active_speed + 1) % 5);   /* 300, 1200, 2400, 9600, max */
        strcpy(msg, "Speed: ");
        strcat(msg, ui_speed_name(active_speed));
        if (active_speed != SPEED_MAX) strcat(msg, " baud");
        show_briefly(msg, 1, 40);
      } else if (IS_KEY_F5(k)) {
        work_drive ^= 1;                  /* shown in the transfer menu's title */
      } else {
        send_terminal_key(k);
      }
    }
  }

  m65_screen_cursor_enable(0);
  net_abort();
}

int main(void)
{
  const char *err = 0;
  unsigned char ip[4];

#ifdef __MEGA65__
  /* Take vectors before anything else */
  m65_own_vectors();
#endif

  /* 1. Standard color startup requirement: Border = 0 (Black), Background = 0 (Black), Text = 1 (White) */
  m65_screen_init();

  /* 2. Initialize Telnet, ANSI, ZModem & XModem subsystems */
  telnet_init(telnet_out_dispatch, telnet_send_dispatch);
  ansi_set_response_fn(telnet_send_dispatch);
  zmodem_init(zmodem_progress_display);
  xmodem_init(xmodem_progress_display);

  m65_screen_puts(2, 2, "MEGA65 BBS CLIENT v1.0", 1);
  m65_screen_puts(2, 4, "Bringing up network stack...", 1);

  /* 3. Bring up mega-net & DHCP */
  if (!net_load(&err)) {
    m65_screen_puts(2, 6, "Network load failed:", 2);
    m65_screen_puts(2, 7, err ? err : "unknown", 2);
    for (;;) ;
  }

  m65_screen_puts(2, 5, "Acquiring DHCP lease...", 1);
  if (!net_dhcp(&err)) {
    m65_screen_puts(2, 6, "DHCP: Offline mode (no lease)", 1);
  }

  /* 4. Load CP437 font from disk into Bank 1 ($11000) */
  m65_screen_puts(2, 6, "Loading CP437 font...", 1);
  cbmdos_load("CP437", boot_drive, FONT_CP437_RAM, 2048);

  /* 5. Initialize Bookmarks */
  work_drive = boot_drive;
  bookmarks_init(boot_drive);

  /* 6. Main application loop */
  for (;;) {
    unsigned char sel = ui_dialing_directory(boot_drive);
    bookmark_t *bm;

    if (sel == 0xff) continue;
    bm = (sel == UI_QUICK_DIAL) ? ui_quick_site() : bookmarks_get(sel);   /* a quick dial is not in the list (2026-10-02) */
    if (!bm) continue;

    m65_screen_cls();
    m65_screen_puts(2, 10, "Connecting to: ", 1);
    m65_screen_puts(17, 10, bm->name, 1);
    m65_screen_puts(2, 12, "Resolving host...", 3);

    if (!net_resolve(bm->host, ip, &err)) {
      m65_screen_puts(2, 14, "DNS Resolution Failed: ", 2);
      m65_screen_puts(25, 14, err ? err : "unknown", 2);
      m65_screen_puts(2, 16, "Press any key to return...", 1);
      ui_wait_key();
      continue;
    }

    m65_screen_puts(2, 13, "Connecting to socket...", 3);
    if (!net_connect(ip, bm->port, &err)) {
      m65_screen_puts(2, 14, "Connection Failed: ", 2);
      m65_screen_puts(21, 14, err ? err : "refused", 2);
      m65_screen_puts(2, 16, "Press any key to return...", 1);
      ui_wait_key();
      continue;
    }

    run_terminal_session(bm);
  }

  return 0;
}

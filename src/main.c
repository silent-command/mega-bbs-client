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
static unsigned char active_drive = 0;
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

static __attribute__((noinline)) void hold_transfer_progress(void)
{
#ifdef __MEGA65__
  /* About 1.8 s, counted by net_poll, with a spin backstop for a stalled
   * frame counter. */
  unsigned int spins = 0;
  unsigned char wraps = 0;
  net_frames = 0;
  while (net_frames < 90) {
    net_poll();
    if (++spins == 0 && ++wraps >= 20) break;
  }
#endif
}

static char transfer_fname[20];
static unsigned long transfer_fsize;

static __attribute__((noinline)) void handle_zmodem_upload(void)
{
  if (ui_file_picker(active_drive, transfer_fname, &transfer_fsize) && transfer_fname[0]) {
    zmodem_send(transfer_fname, active_drive, transfer_fsize);
    hold_transfer_progress();
  }
}

static __attribute__((noinline)) void handle_xmodem_upload(void)
{
  if (ui_file_picker(active_drive, transfer_fname, &transfer_fsize) && transfer_fname[0]) {
    xmodem_send(transfer_fname, active_drive, transfer_fsize);
    hold_transfer_progress();
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
    zmodem_receive(active_drive);
    hold_transfer_progress();
  } else if (act == TRANSFER_ACT_X_DOWN) {
    unsigned char pr_row = (m65_screen_rows() > 25) ? 14 : 10;
    transfer_fname[0] = 0;
    if (ui_read_line(pr_row, "Save filename: ", transfer_fname, 16) && transfer_fname[0]) {
      xmodem_receive(transfer_fname, active_drive);
      hold_transfer_progress();
    }
  }
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
  m65_screen_set_res(active_res);
  m65_screen_set_emul(active_emul);
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
  unsigned int n;

  active_bbs_name = bm->name;
  (void)active_bbs_name;
  active_emul = bm->emul;
  active_res = bm->res;
  active_drive = bm->drive;

  /* Configure screen and emulation */
  m65_screen_set_res(active_res);
  m65_screen_set_emul(active_emul);
  m65_screen_cls();

  ansi_reset();
  petscii_reset();
  telnet_reset();
  telnet_set_terminal_type(active_emul == EMUL_ANSI ? "ANSI" : "PETSCII");
  telnet_set_window_size(m65_screen_cols(), m65_screen_rows());
  telnet_send_naws(m65_screen_cols(), m65_screen_rows());
  telnet_send_init_negotiation();

  for (;;) {
    unsigned char k;

    net_poll();

    /* Receive incoming data from socket */
    n = net_recv(rx_buf, RX_BUF_SIZE);
    if (n > 0) {
      telnet_feed(rx_buf, n);

      /* Auto-detect ZModem download */
      if (telnet_check_zmodem()) {
        telnet_clear_zmodem();
        zmodem_receive(active_drive);
        hold_transfer_progress();
        /* Redraw terminal after transfer */
        m65_screen_set_res(active_res);
        m65_screen_set_emul(active_emul);
      }
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
      } else if (IS_KEY_F5(k)) {
        handle_terminal_transfer();
      } else {
        send_terminal_key(k);
      }
    }
  }

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
  m65_screen_puts(2, 4, "Bringing up network stack...", 7);

  /* 3. Bring up mega-net & DHCP */
  if (!net_load(&err)) {
    m65_screen_puts(2, 6, "Network load failed:", 2);
    m65_screen_puts(2, 7, err ? err : "unknown", 2);
    for (;;) ;
  }

  m65_screen_puts(2, 5, "Acquiring DHCP lease...", 7);
  if (!net_dhcp(&err)) {
    m65_screen_puts(2, 6, "DHCP: Offline mode (no lease)", 7);
  }

  /* 4. Load CP437 font from disk into Bank 1 ($11000) */
  m65_screen_puts(2, 6, "Loading CP437 font...", 7);
  cbmdos_load("CP437", boot_drive, FONT_CP437_RAM, 2048);

  /* 5. Initialize Bookmarks */
  bookmarks_init(boot_drive);

  /* 6. Main application loop */
  for (;;) {
    unsigned char sel = ui_dialing_directory(boot_drive);
    bookmark_t *bm;

    if (sel == 0xff) continue;
    bm = bookmarks_get(sel);
    if (!bm) continue;

    m65_screen_cls();
    m65_screen_puts(2, 10, "Connecting to: ", 1);
    m65_screen_puts(17, 10, bm->name, 7);
    m65_screen_puts(2, 12, "Resolving host...", 3);

    if (!net_resolve(bm->host, ip, &err)) {
      m65_screen_puts(2, 14, "DNS Resolution Failed: ", 2);
      m65_screen_puts(25, 14, err ? err : "unknown", 2);
      m65_screen_puts(2, 16, "Press any key to return...", 7);
      ui_wait_key();
      continue;
    }

    m65_screen_puts(2, 13, "Connecting to socket...", 3);
    if (!net_connect(ip, bm->port, &err)) {
      m65_screen_puts(2, 14, "Connection Failed: ", 2);
      m65_screen_puts(21, 14, err ? err : "refused", 2);
      m65_screen_puts(2, 16, "Press any key to return...", 7);
      ui_wait_key();
      continue;
    }

    run_terminal_session(bm);
  }

  return 0;
}

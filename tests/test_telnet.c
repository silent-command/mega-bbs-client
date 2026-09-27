#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "telnet.h"
#include "netutil.h"

static unsigned char last_out_char = 0;
static unsigned int out_char_count = 0;
static unsigned char out_buf[64];
static unsigned char last_tx_buf[64];
static unsigned int last_tx_len = 0;
static unsigned char tx_all[256];
static unsigned int tx_all_len = 0;

/* The stream the transfer paths read from. */
static unsigned char rx_q[256];
static unsigned int rx_len = 0, rx_pos = 0;

static void mock_out(unsigned char c)
{
  last_out_char = c;
  if (out_char_count < sizeof(out_buf)) out_buf[out_char_count] = c;
  out_char_count++;
}

static void mock_tx(const unsigned char *data, unsigned int len)
{
  if (len < sizeof(last_tx_buf)) {
    memcpy(last_tx_buf, data, len);
    last_tx_len = len;
  }
}

unsigned int net_send(const unsigned char *p, unsigned int n)
{
  if (tx_all_len + n <= sizeof(tx_all)) memcpy(tx_all + tx_all_len, p, n);
  tx_all_len += n;
  return n;
}
unsigned char net_send_all(const unsigned char *p, unsigned int n) { net_send(p, n); return 1; }
unsigned int net_recv(unsigned char *buf, unsigned int cap)
{
  (void)cap;
  if (rx_pos >= rx_len) return 0;
  buf[0] = rx_q[rx_pos++];
  return 1;
}
void net_poll(void) {}
unsigned char net_alive(void) { return 1; }

static void queue(const unsigned char *d, unsigned int n)
{
  memcpy(rx_q, d, n);
  rx_len = n;
  rx_pos = 0;
}

int main(void)
{
  printf("Testing Telnet engine...\n");

  telnet_init(mock_out, mock_tx);

  /* Test normal stream pass-through */
  {
    const unsigned char msg[] = "Hello BBS";
    telnet_feed(msg, strlen((const char *)msg));
    assert(out_char_count == strlen((const char *)msg));
    assert(last_out_char == 'S');
  }

  /* A raw board: its first bytes are not a telnet command, so the link
   * is raw for good: our options are never sent, 0xFF followed by data
   * is two characters (also later, when art happens to put pi next to a
   * byte in the command range), and a transfer sees bytes as they are. */
  {
    unsigned char b, got[8], n = 0;
    telnet_reset();
    tx_all_len = 0;
    last_tx_len = 0;
    telnet_send_init_negotiation();
    telnet_send_naws(40, 25);
    assert(tx_all_len == 0 && last_tx_len == 0);             /* nothing sent yet */
    out_char_count = 0;
    telnet_feed((const unsigned char *)"\x93\xff\x41", 3);
    assert(out_char_count == 3 && out_buf[1] == 0xff && out_buf[2] == 0x41);
    assert(!telnet_is_telnet());
    telnet_feed((const unsigned char *)"\xff\xfd\x03", 3);              /* looks like IAC DO SGA, but the link is raw */
    assert(!telnet_is_telnet() && out_char_count == 6 && last_tx_len == 0);
    queue((const unsigned char *)"\xff\xff\xfb\x01", 4);
    while (telnet_rx_byte(&b)) got[n++] = b;
    assert(n == 4 && got[0] == 0xff && got[1] == 0xff && got[2] == 0xfb);   /* raw: all four are data */
    tx_all_len = 0;
    assert(telnet_tx_data((const unsigned char *)"\xff", 1) && tx_all_len == 1);   /* and nothing is doubled */
  }

  /* A telnet server: it negotiates first, which releases the held options */
  {
    telnet_reset();
    last_tx_len = 0;
    telnet_send_init_negotiation();
    telnet_send_naws(40, 25);
    telnet_feed((const unsigned char *)"\xff\xfd\x03", 3);              /* IAC DO SGA as the first bytes */
    assert(telnet_is_telnet());
    assert(last_tx_len == 3 && last_tx_buf[1] == TELNET_WILL);          /* the answer to DO SGA, after the held options and NAWS */
  }

  /* Test IAC DO BINARY negotiation, on a fresh link whose first bytes it is */
  {
    const unsigned char do_bin[] = { TELNET_IAC, TELNET_DO, TELOPT_BINARY };
    telnet_reset();
    last_tx_len = 0;
    telnet_feed(do_bin, 3);
    assert(last_tx_len == 3);
    assert(last_tx_buf[0] == TELNET_IAC);
    assert(last_tx_buf[1] == TELNET_WILL);
    assert(last_tx_buf[2] == TELOPT_BINARY);
    assert(!telnet_binary());
  }

  /* Test IAC DO NAWS negotiation with custom window size */
  {
    const unsigned char do_naws[] = { TELNET_IAC, TELNET_DO, TELOPT_NAWS };
    telnet_set_window_size(80, 50);
    last_tx_len = 0;
    telnet_feed(do_naws, 3);
    /* Should have responded with WILL NAWS and subnegotiation */
    assert(last_tx_len == 9);
    assert(last_tx_buf[0] == TELNET_IAC);
    assert(last_tx_buf[1] == TELNET_SB);
    assert(last_tx_buf[2] == TELOPT_NAWS);
    assert(last_tx_buf[4] == 80);
    assert(last_tx_buf[6] == 50);
  }

  /* The opening negotiation asks for BINARY both ways */
  {
    last_tx_len = 0;
    telnet_send_init_negotiation();
    assert(last_tx_len == 18);
    assert(last_tx_buf[13] == TELNET_WILL && last_tx_buf[14] == TELOPT_BINARY);
    assert(last_tx_buf[16] == TELNET_DO && last_tx_buf[17] == TELOPT_BINARY);
  }

  /* ZModem auto-detect: only the sender's ZRQINIT starts a download */
  {
    telnet_reset();
    assert(!telnet_check_zmodem());
    telnet_feed((const unsigned char *)"**\x18" "B00", 6);
    assert(telnet_check_zmodem());
    telnet_clear_zmodem();
    telnet_reset();

    /* A remote rz answering our upload sends ZRINIT: not a download */
    telnet_feed((const unsigned char *)"**\x18" "B01", 6);
    assert(!telnet_check_zmodem());
    telnet_reset();

    /* Text that merely looks like the old loose patterns */
    telnet_feed((const unsigned char *)"rz\r", 3);
    assert(!telnet_check_zmodem());
    telnet_feed((const unsigned char *)"**B00", 5);
    assert(!telnet_check_zmodem());
    telnet_reset();

    /* Binary ZRQINIT, ZBIN and ZBIN32 */
    telnet_feed((const unsigned char *)"*\x18" "A\x00", 4);
    assert(telnet_check_zmodem());
    telnet_reset();
    telnet_feed((const unsigned char *)"*\x18" "C\x00", 4);
    assert(telnet_check_zmodem());
    telnet_reset();

    /* A ZFILE on its own does not */
    telnet_feed((const unsigned char *)"*\x18" "C\x04", 4);
    assert(!telnet_check_zmodem());
    telnet_reset();
  }

  /* Bytes that could start a header are held back from the screen until
   * the next byte settles it; a pause releases them. */
  {
    telnet_reset();
    out_char_count = 0;
    telnet_feed((const unsigned char *)"abc*", 4);
    assert(out_char_count == 3);                     /* the star waits */
    telnet_feed((const unsigned char *)"d", 1);
    assert(out_char_count == 5 && out_buf[3] == '*' && out_buf[4] == 'd');
    telnet_feed((const unsigned char *)"**\x18" "B0", 5);
    assert(out_char_count == 5);                     /* five bytes of a possible ZRQINIT held */
    telnet_feed((const unsigned char *)"x", 1);
    assert(out_char_count == 11);                    /* it was not one: all six shown */
    telnet_feed((const unsigned char *)"*", 1);
    telnet_idle();
    assert(out_char_count == 12);
    telnet_feed((const unsigned char *)"**\x18" "B00", 6);
    assert(out_char_count == 12 && telnet_check_zmodem());   /* a real one: nothing shown */
    telnet_reset();
  }

  /* Detection hands the header and the rest of the receive to the
   * transfer, which reads them before the network. */
  {
    unsigned char b;
    out_char_count = 0;
    telnet_feed((const unsigned char *)"hi**\x18" "B00000000000000\r\n\x11" "tail", 27);
    assert(telnet_check_zmodem());
    assert(out_char_count == 2);                     /* "hi" reached the screen, the header did not */
    queue((const unsigned char *)"net", 3);
    assert(telnet_rx_byte(&b) && b == '*');
    assert(telnet_rx_byte(&b) && b == '*');
    assert(telnet_rx_byte(&b) && b == 0x18);
    {
      unsigned char i;
      for (i = 0; i < 18; i++) assert(telnet_rx_byte(&b));   /* B + 14 hex digits + CR LF XON */
      assert(b == 0x11);
    }
    assert(telnet_rx_byte(&b) && b == 't');
    assert(telnet_rx_byte(&b) && b == 'a');
    assert(telnet_rx_byte(&b) && b == 'i');
    assert(telnet_rx_byte(&b) && b == 'l');
    assert(telnet_rx_byte(&b) && b == 'n');           /* then the network */
    assert(telnet_rx_byte(&b) && b == 'e');
    assert(telnet_rx_byte(&b) && b == 't');
    assert(!telnet_rx_byte(&b));
    telnet_clear_zmodem();
    telnet_reset();
  }

  /* During a transfer on a telnet link, IAC IAC is one 0xFF, a NOP
   * vanishes, an option request is answered, and a ZRQINIT-shaped run of
   * data is not detected. */
  {
    telnet_feed((const unsigned char *)"\xff\xfd\x03", 3);      /* the peer is a telnet server */
    static const unsigned char stream[] = {
      0x41, 0xff, 0xff, 0x42, 0xff, TELNET_NOP, 0x43,
      0xff, TELNET_WILL, TELOPT_ECHO, 0x44,
      '*', '*', 0x18, 'B', '0', '0', 0x45
    };
    unsigned char b, got[32], n = 0;
    queue(stream, sizeof(stream));
    last_tx_len = 0;
    while (telnet_rx_byte(&b)) got[n++] = b;
    assert(n == 12);
    assert(got[0] == 0x41 && got[1] == 0xff && got[2] == 0x42 && got[3] == 0x43 && got[4] == 0x44);
    assert(got[5] == '*' && got[10] == '0' && got[11] == 0x45);
    assert(!telnet_check_zmodem());
    assert(last_tx_len == 3 && last_tx_buf[1] == TELNET_DO && last_tx_buf[2] == TELOPT_ECHO);
    telnet_reset();
  }

  /* An IAC split across two reads keeps its state */
  {
    unsigned char b;
    telnet_feed((const unsigned char *)"\xff\xfd\x03", 3);
    queue((const unsigned char *)"\xff", 1);
    assert(!telnet_rx_byte(&b));
    queue((const unsigned char *)"\xff" "Z", 2);
    assert(telnet_rx_byte(&b) && b == 0xff);
    assert(telnet_rx_byte(&b) && b == 'Z');
    telnet_reset();
  }

  /* Outbound data doubles 0xFF and nothing else, once the peer is telnet */
  {
    telnet_feed((const unsigned char *)"\xff\xfd\x03", 3);
    static const unsigned char data[] = { 0x01, 0xff, 0x02, 0xff, 0xff, 0x03 };
    static const unsigned char want[] = { 0x01, 0xff, 0xff, 0x02, 0xff, 0xff, 0xff, 0xff, 0x03 };
    tx_all_len = 0;
    assert(telnet_tx_data(data, sizeof(data)));
    assert(tx_all_len == sizeof(want));
    assert(memcmp(tx_all, want, sizeof(want)) == 0);
    tx_all_len = 0;
    assert(telnet_tx_data((const unsigned char *)"\xff", 1));
    assert(tx_all_len == 2);
  }

  printf("Telnet tests passed successfully!\n");
  return 0;
}

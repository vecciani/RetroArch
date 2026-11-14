/*  RetroArch - A frontend for libretro.
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* Pads of an Oberon Remote (github.com/SamsidParty/OberonRemote) on the
 * local network, read over its WebSocket, with buttons numbered as in
 * xinput_joypad.c. The XInput pads, which this driver keeps running,
 * stay on pads 0-3; the remote's come after them. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <retro_miscellaneous.h>
#include <compat/strl.h>
#include <features/features_cpu.h>
#include <string/stdstring.h>
#include <net/net_compat.h>
#include <net/net_socket.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../configuration.h"
#include "../../runloop.h"
#include "../../tasks/tasks_internal.h"
#include "../input_driver.h"
#include "../../verbosity.h"

#define OBERON_MAX_PADS     4
/* the XInput pads come first */
#define OBERON_FIRST_PAD    4
#define OBERON_NUM_BUTTONS  11
#define OBERON_NUM_AXES     6
#define OBERON_PORT         26401
#define OBERON_PAD_BYTES    25
#define OBERON_TIMEOUT_USEC 3000000
#define OBERON_RETRY_USEC   2000000
#define OBERON_NAME         "Oberon Gamepad"

/* Byte 13 of a pad's 25 is the high byte of the word, byte 14 the low. */
#define OBERON_DPAD_UP      0x0010
#define OBERON_DPAD_DOWN    0x0008
#define OBERON_DPAD_LEFT    0x0004
#define OBERON_DPAD_RIGHT   0x0002

#define OBERON_WS_BINARY        0x2
#define OBERON_WS_CLOSE         0x8
#define OBERON_WS_PING          0x9
#define OBERON_WS_PONG          0xA

/* the driver whose pads share the ports */
#define OBERON_INNER        (&xinput_joypad)

enum oberon_phase
{
   OBERON_IDLE = 0,
   OBERON_CONNECTING,
   OBERON_UPGRADING,
   OBERON_RUNNING
};

typedef struct oberon_pad
{
   int16_t  axes[OBERON_NUM_AXES];
   uint16_t buttons;
   bool     connected;
} oberon_pad_t;

typedef struct oberon
{
   struct addrinfo *addr;
   retro_time_t deadline;
   size_t rx_len;
   size_t tx_len;
   size_t tx_pos;
   size_t skip;
   uint32_t mask_seed;
   int fd;
   enum oberon_phase phase;
   oberon_pad_t pads[OBERON_MAX_PADS];
   uint8_t rumble[OBERON_MAX_PADS][2];
   bool named[OBERON_MAX_PADS];
   /* Some remotes report all four pads present: one counts once it moves. */
   bool seen[OBERON_MAX_PADS];
   bool awaiting;
   bool up;
   bool reported;
   unsigned port;
   char address[64];
   char host[64];
   uint8_t rx[1024];
   uint8_t tx[512];
} oberon_t;

/* xinput_joypad.c's order */
static const uint16_t oberon_button_map[OBERON_NUM_BUTTONS] = {
   0x8000, /* A */
   0x4000, /* B */
   0x2000, /* X */
   0x1000, /* Y */
   0x0800, /* left shoulder */
   0x0400, /* right shoulder */
   0x0200, /* start */
   0x0100, /* back */
   0x0040, /* left thumb */
   0x0020, /* right thumb */
   0x0001  /* guide */
};

static oberon_t oberon_st;

static void oberon_close(oberon_t *ob)
{
   unsigned i;

   if (ob->fd >= 0)
      socket_close(ob->fd);
   if (ob->addr)
      freeaddrinfo_retro(ob->addr);

   ob->fd       = -1;
   ob->addr     = NULL;
   ob->phase    = OBERON_IDLE;
   ob->rx_len   = 0;
   ob->tx_len   = 0;
   ob->tx_pos   = 0;
   ob->skip     = 0;
   ob->awaiting = false;
   for (i = 0; i < OBERON_MAX_PADS; i++)
      memset(&ob->pads[i], 0, sizeof(ob->pads[i]));
}

static void oberon_notify(const char *msg)
{
   runloop_msg_queue_push(msg, strlen(msg), 1, 180, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

static void oberon_fail(oberon_t *ob, const char *why)
{
   /* once a streak: the remote may be off for hours */
   if (!ob->reported)
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "Oberon: %s:%u %s, retrying.",
            ob->host, ob->port, why);
      RARCH_WARN("[Oberon] %s\n", msg);
      oberon_notify(msg);
   }
   ob->reported = true;
   ob->up       = false;
   oberon_close(ob);
   ob->deadline = cpu_features_get_time_usec() + OBERON_RETRY_USEC;
}

/* Dotted IPv4 only, so that resolving it never waits. */
static bool oberon_host_valid(const char *s)
{
   unsigned dots   = 0;
   unsigned digits = 0;

   if (!s || !*s || string_is_equal(s, "0.0.0.0"))
      return false;

   for (; *s; s++)
   {
      if (*s == '.')
      {
         if (!digits)
            return false;
         dots++;
         digits = 0;
      }
      else if (*s >= '0' && *s <= '9')
      {
         if (++digits > 3)
            return false;
      }
      else
         return false;
   }

   return dots == 3 && digits > 0;
}

/* As typed, or as Oberon shows it: "192.168.0.5", "192.168.0.5:26401"
 * or "ws://192.168.0.5:26401/". */
static bool oberon_parse_address(const char *in, char *host, size_t len,
      unsigned *port)
{
   char *colon;
   size_t n;

   while (*in == ' ')
      in++;
   if (!strncmp(in, "ws://", 5) || !strncmp(in, "WS://", 5))
      in += 5;

   strlcpy(host, in, len);
   n = strlen(host);
   while (n && (host[n - 1] == '/' || host[n - 1] == ' '))
      host[--n] = '\0';

   *port = OBERON_PORT;
   if ((colon = strchr(host, ':')))
   {
      char *end;
      unsigned long p = strtoul(colon + 1, &end, 10);
      if (*end || !p || p > 65535)
         return false;
      *port  = (unsigned)p;
      *colon = '\0';
   }

   return oberon_host_valid(host);
}

/* Client frames are masked. Dropped when the queue is full. */
static bool oberon_queue_frame(oberon_t *ob, unsigned opcode,
      const uint8_t *payload, size_t len)
{
   size_t i;
   uint8_t mask[4];
   uint8_t *out = ob->tx + ob->tx_len;

   if (len > 125 || ob->tx_len + 6 + len > sizeof(ob->tx))
      return false;

   ob->mask_seed = ob->mask_seed * 1664525u + 1013904223u;
   mask[0]       = (uint8_t)(ob->mask_seed >> 24);
   mask[1]       = (uint8_t)(ob->mask_seed >> 16);
   mask[2]       = (uint8_t)(ob->mask_seed >> 8);
   mask[3]       = (uint8_t)(ob->mask_seed);

   out[0]        = (uint8_t)(0x80 | opcode);
   out[1]        = (uint8_t)(0x80 | len);
   memcpy(out + 2, mask, sizeof(mask));
   for (i = 0; i < len; i++)
      out[6 + i] = payload[i] ^ mask[i & 3];

   ob->tx_len   += 6 + len;
   return true;
}

static bool oberon_flush(oberon_t *ob)
{
   ssize_t sent;

   if (ob->tx_pos >= ob->tx_len)
      return true;

   sent = socket_send_all_nonblocking(ob->fd, ob->tx + ob->tx_pos,
         ob->tx_len - ob->tx_pos, true);
   if (sent < 0)
      return false;

   ob->tx_pos += (size_t)sent;
   if (ob->tx_pos >= ob->tx_len)
   {
      ob->tx_pos = 0;
      ob->tx_len = 0;
   }
   return true;
}

static void oberon_consume(oberon_t *ob, size_t len)
{
   if (len > ob->rx_len)
      len = ob->rx_len;
   ob->rx_len -= len;
   if (ob->rx_len)
      memmove(ob->rx, ob->rx + len, ob->rx_len);
}

static void oberon_begin_upgrade(oberon_t *ob)
{
#ifdef TCP_NODELAY
   int one = 1;
   setsockopt(ob->fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
#endif

   /* The remote never checks the key. */
   ob->tx_pos   = 0;
   ob->tx_len   = (size_t)snprintf((char*)ob->tx, sizeof(ob->tx),
         "GET / HTTP/1.1\r\n"
         "Host: %s:%d\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Key: UmV0cm9BcmNoIE9iZXJvbg==\r\n"
         "Sec-WebSocket-Version: 13\r\n"
         "\r\n",
         ob->host, ob->port);
   ob->rx_len   = 0;
   ob->phase    = OBERON_UPGRADING;
   ob->deadline = cpu_features_get_time_usec() + OBERON_TIMEOUT_USEC;
}

static void oberon_start(oberon_t *ob)
{
   ob->fd = socket_init((void**)&ob->addr, (uint16_t)ob->port, ob->host,
         SOCKET_TYPE_STREAM, AF_INET);
   if (ob->fd < 0)
   {
      oberon_fail(ob, "has no socket");
      return;
   }

   switch (socket_connect_begin(ob->fd, ob->addr))
   {
      case SOCKET_CONNECT_READY:
         oberon_begin_upgrade(ob);
         break;
      case SOCKET_CONNECT_PENDING:
         ob->phase    = OBERON_CONNECTING;
         ob->deadline = cpu_features_get_time_usec() + OBERON_TIMEOUT_USEC;
         break;
      default:
         oberon_fail(ob, "refused the connection");
         break;
   }
}

static void oberon_read_pads(oberon_t *ob, const uint8_t *packet)
{
   unsigned i, j;

   for (i = 0; i < OBERON_MAX_PADS; i++)
   {
      const uint8_t *in = packet + i * OBERON_PAD_BYTES;
      oberon_pad_t *pad = &ob->pads[i];

      /* The first byte is also the packet's type: pad 0 always reads
       * as present. */
      if (in[0] != 0xFF)
      {
         memset(pad, 0, sizeof(*pad));
         continue;
      }

      for (j = 0; j < OBERON_NUM_AXES; j++)
      {
         int val = (int16_t)((uint16_t)in[1 + j * 2]
               | ((uint16_t)in[2 + j * 2] << 8));
         /* sticks come with Y down, XInput has it up */
         if (j == 1 || j == 3)
            val  = -val;
         if (val > 32767)
            val  = 32767;
         else if (val < -32767)
            val  = -32767;
         pad->axes[j] = (int16_t)val;
      }

      pad->buttons   = (uint16_t)(((uint16_t)in[13] << 8) | in[14]);
      pad->connected = true;
   }
}

/* False when the connection is over. */
static bool oberon_parse(oberon_t *ob)
{
   if (ob->phase == OBERON_UPGRADING)
   {
      size_t i;
      size_t end = 0;

      for (i = 0; i + 4 <= ob->rx_len; i++)
      {
         if (!memcmp(ob->rx + i, "\r\n\r\n", 4))
         {
            end = i + 4;
            break;
         }
      }

      if (!end)
         return ob->rx_len < sizeof(ob->rx);
      if (end < 12 || memcmp(ob->rx, "HTTP/1.1 101", 12))
         return false;

      oberon_consume(ob, end);
      ob->phase    = OBERON_RUNNING;
      ob->awaiting = false;
   }

   for (;;)
   {
      size_t i;
      size_t hdr = 2;
      size_t len;
      uint8_t *payload;

      if (ob->skip)
      {
         size_t n  = (ob->skip < ob->rx_len) ? ob->skip : ob->rx_len;
         oberon_consume(ob, n);
         ob->skip -= n;
         if (ob->skip)
            break;
      }

      if (ob->rx_len < 2)
         break;

      len = ob->rx[1] & 0x7F;
      if (len == 126)
         hdr = 4;
      else if (len == 127)
         hdr = 10;
      if (ob->rx[1] & 0x80)
         hdr += 4;
      if (ob->rx_len < hdr)
         break;

      if (len == 126)
         len = ((size_t)ob->rx[2] << 8) | ob->rx[3];
      else if (len == 127)
      {
         /* nothing the remote sends is this long */
         if (ob->rx[2] || ob->rx[3] || ob->rx[4] || ob->rx[5])
            return false;
         len = ((size_t)ob->rx[6] << 24) | ((size_t)ob->rx[7] << 16)
             | ((size_t)ob->rx[8] << 8)  |  (size_t)ob->rx[9];
      }

      /* does not fit: let it pass by */
      if (hdr + len > sizeof(ob->rx))
      {
         oberon_consume(ob, hdr);
         ob->skip = len;
         continue;
      }

      if (ob->rx_len < hdr + len)
         break;

      payload = ob->rx + hdr;
      if (ob->rx[1] & 0x80)
         for (i = 0; i < len; i++)
            payload[i] ^= ob->rx[hdr - 4 + (i & 3)];

      switch (ob->rx[0] & 0x0F)
      {
         case OBERON_WS_CLOSE:
            return false;
         case OBERON_WS_PING:
            oberon_queue_frame(ob, OBERON_WS_PONG, payload, (len > 125) ? 125 : len);
            break;
         case OBERON_WS_BINARY:
            /* its name comes too, as a 0x0A packet */
            if (len >= OBERON_MAX_PADS * OBERON_PAD_BYTES && payload[0] == 0xFF)
            {
               oberon_read_pads(ob, payload);
               ob->awaiting = false;
               if (!ob->up)
               {
                  char msg[128];
                  snprintf(msg, sizeof(msg), "Oberon: connected to %s:%u.",
                        ob->host, ob->port);
                  RARCH_LOG("[Oberon] %s\n", msg);
                  oberon_notify(msg);
               }
               ob->up       = true;
               ob->reported = false;
            }
            break;
         default:
            break;
      }

      oberon_consume(ob, hdr + len);
   }

   return true;
}

/* Reads until the socket has no more. False when the connection is over. */
static bool oberon_receive(oberon_t *ob)
{
   for (;;)
   {
      bool err     = false;
      size_t space = sizeof(ob->rx) - ob->rx_len;
      ssize_t got  = socket_receive_all_nonblocking(ob->fd, &err,
            ob->rx + ob->rx_len, space);

      if (err)
         return false;
      if (got <= 0)
         break;

      ob->rx_len  += (size_t)got;
      if (!oberon_parse(ob))
         return false;
      if ((size_t)got < space)
         break;
   }
   return true;
}

static void oberon_network_poll(oberon_t *ob)
{
   settings_t *settings = config_get_ptr();
   const char *address  = settings->arrays.input_oberon_address;
   retro_time_t now     = cpu_features_get_time_usec();

   if (!string_is_equal(address, ob->address))
   {
      oberon_close(ob);
      strlcpy(ob->address, address, sizeof(ob->address));
      ob->up       = false;
      ob->reported = false;
      ob->deadline = now;
      ob->host[0]  = '\0';
      if (     *ob->address
            && !oberon_parse_address(ob->address, ob->host,
               sizeof(ob->host), &ob->port))
      {
         char msg[128];
         ob->host[0] = '\0';
         snprintf(msg, sizeof(msg), "Oberon: \"%s\" is not an IPv4 address.",
               ob->address);
         oberon_notify(msg);
      }
   }

   switch (ob->phase)
   {
      case OBERON_IDLE:
         if (now >= ob->deadline && *ob->host)
            oberon_start(ob);
         break;
      case OBERON_CONNECTING:
         switch (socket_connect_step(ob->fd, ob->addr))
         {
            case SOCKET_CONNECT_READY:
               oberon_begin_upgrade(ob);
               break;
            case SOCKET_CONNECT_PENDING:
               if (now > ob->deadline)
                  oberon_fail(ob, "does not answer");
               break;
            default:
               oberon_fail(ob, "refused the connection");
               break;
         }
         break;
      case OBERON_UPGRADING:
      case OBERON_RUNNING:
         if (!oberon_flush(ob) || !oberon_receive(ob))
         {
            oberon_fail(ob, "closed the connection");
            break;
         }

         /* One request out at a time: the answer to this one is read
          * on the next poll. */
         if (     ob->phase == OBERON_RUNNING
               && !ob->awaiting
               && !ob->tx_len)
         {
            unsigned i;
            uint8_t request[17];

            memset(request, 0, sizeof(request));
            request[0] = 0xFA;
            for (i = 0; i < OBERON_MAX_PADS; i++)
            {
               request[i * 4 + 1] = ob->rumble[i][0];
               request[i * 4 + 2] = ob->rumble[i][1];
            }

            if (oberon_queue_frame(ob, OBERON_WS_BINARY, request, sizeof(request)))
            {
               ob->awaiting = true;
               ob->deadline = now + OBERON_TIMEOUT_USEC;
            }
         }

         if (!oberon_flush(ob))
            oberon_fail(ob, "closed the connection");
         else if (now > ob->deadline)
            oberon_fail(ob, "stopped answering");
         break;
   }
}

/* NULL unless @pad is one of the remote's, there and counted. */
static oberon_pad_t *oberon_get(unsigned pad)
{
   unsigned i;

   if (pad < OBERON_FIRST_PAD || pad >= OBERON_FIRST_PAD + OBERON_MAX_PADS)
      return NULL;
   i = pad - OBERON_FIRST_PAD;
   if (!oberon_st.pads[i].connected || (i && !oberon_st.seen[i]))
      return NULL;
   return &oberon_st.pads[i];
}

static int32_t oberon_button_state(const oberon_pad_t *pad, uint16_t joykey)
{
   unsigned hat_dir = GET_HAT_DIR(joykey);

   if (hat_dir)
   {
      switch (hat_dir)
      {
         case HAT_UP_MASK:
            return (pad->buttons & OBERON_DPAD_UP);
         case HAT_DOWN_MASK:
            return (pad->buttons & OBERON_DPAD_DOWN);
         case HAT_LEFT_MASK:
            return (pad->buttons & OBERON_DPAD_LEFT);
         case HAT_RIGHT_MASK:
            return (pad->buttons & OBERON_DPAD_RIGHT);
         default:
            break;
      }
   }
   else if (joykey < OBERON_NUM_BUTTONS)
      return (pad->buttons & oberon_button_map[joykey]);
   return 0;
}

static int16_t oberon_axis_state(const oberon_pad_t *pad, uint32_t joyaxis)
{
   int16_t val;

   /* triggers (axes 4,5) cannot be negative */
   if (AXIS_NEG_GET(joyaxis) <= 3)
   {
      val = pad->axes[AXIS_NEG_GET(joyaxis)];
      return (val < 0) ? val : 0;
   }
   else if (AXIS_POS_GET(joyaxis) <= 5)
   {
      val = pad->axes[AXIS_POS_GET(joyaxis)];
      return (val > 0) ? val : 0;
   }
   return 0;
}

static void *oberon_joypad_init(void *data)
{
   memset(&oberon_st, 0, sizeof(oberon_st));
   oberon_st.fd        = -1;
   oberon_st.mask_seed = (uint32_t)cpu_features_get_time_usec();

   if (!network_init())
      RARCH_WARN("[Oberon] No network.\n");

   OBERON_INNER->init(data);

   return (void*)-1;
}

static void oberon_joypad_destroy(void)
{
   OBERON_INNER->destroy();
   oberon_close(&oberon_st);
   memset(&oberon_st, 0, sizeof(oberon_st));
   oberon_st.fd = -1;
}

static bool oberon_joypad_query_pad(unsigned pad)
{
   if (pad < OBERON_FIRST_PAD)
      return OBERON_INNER->query_pad(pad);
   return oberon_get(pad) != NULL;
}

static const char *oberon_joypad_name(unsigned pad)
{
   if (pad < OBERON_FIRST_PAD)
      return OBERON_INNER->name(pad);
   return oberon_get(pad) ? OBERON_NAME : NULL;
}

static int32_t oberon_joypad_button(unsigned pad, uint16_t joykey)
{
   oberon_pad_t *p;

   if (pad < OBERON_FIRST_PAD)
      return OBERON_INNER->button(pad, joykey);
   if (!(p = oberon_get(pad)))
      return 0;
   return oberon_button_state(p, joykey) ? 1 : 0;
}

static void oberon_joypad_get_buttons(unsigned pad, input_bits_t *state)
{
   unsigned i;
   oberon_pad_t *p;

   if (pad < OBERON_FIRST_PAD)
   {
      OBERON_INNER->get_buttons(pad, state);
      return;
   }

   BIT256_CLEAR_ALL_PTR(state);
   if (!(p = oberon_get(pad)))
      return;
   for (i = 0; i < OBERON_NUM_BUTTONS; i++)
      if (p->buttons & oberon_button_map[i])
         BIT256_SET_PTR(state, i);
}

static int16_t oberon_joypad_axis(unsigned pad, uint32_t joyaxis)
{
   oberon_pad_t *p;

   if (pad < OBERON_FIRST_PAD)
      return OBERON_INNER->axis(pad, joyaxis);
   if (!(p = oberon_get(pad)))
      return 0;
   return oberon_axis_state(p, joyaxis);
}

static int16_t oberon_joypad_state(
      rarch_joypad_info_t *joypad_info,
      const uint16_t *joykeys,
      const uint32_t *joyaxes,
      unsigned port)
{
   int i;
   int16_t ret = 0;
   oberon_pad_t *p;

   if (joypad_info->joy_idx < OBERON_FIRST_PAD)
      return OBERON_INNER->state(joypad_info, joykeys, joyaxes, port);
   if (!(p = oberon_get(joypad_info->joy_idx)))
      return 0;

   for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
   {
      const uint16_t joykey  = joykeys[i];
      const uint32_t joyaxis = joyaxes[i];
      if (     joykey != NO_BTN
            && oberon_button_state(p, joykey))
         ret |= (1 << i);
      else if (joyaxis != AXIS_NONE &&
            ((float)abs(oberon_axis_state(p, joyaxis))
             / 0x8000) > joypad_info->axis_threshold)
         ret |= (1 << i);
   }

   return ret;
}

/* Anything off rest: a button, or a stick or trigger past a quarter. */
static bool oberon_pad_moved(const oberon_pad_t *p)
{
   unsigned j;

   if (p->buttons)
      return true;
   for (j = 0; j < OBERON_NUM_AXES; j++)
      if (abs(p->axes[j]) > 0x2000)
         return true;
   return false;
}

static void oberon_joypad_poll(void)
{
   unsigned i;
   oberon_t *ob = &oberon_st;

   OBERON_INNER->poll();
   oberon_network_poll(ob);

   for (i = 0; i < OBERON_MAX_PADS; i++)
   {
      bool there;

      if (!ob->pads[i].connected)
         ob->seen[i] = false;
      else if (!ob->seen[i] && oberon_pad_moved(&ob->pads[i]))
         ob->seen[i] = true;

      there = oberon_get(OBERON_FIRST_PAD + i) != NULL;
      if (there && !ob->named[i])
      {
         input_autoconfigure_connect(OBERON_NAME, NULL, NULL,
               oberon_joypad.ident, OBERON_FIRST_PAD + i, 0, 0);
         ob->named[i] = true;
      }
      else if (!there && ob->named[i])
      {
         input_autoconfigure_disconnect(OBERON_FIRST_PAD + i, OBERON_NAME);
         ob->named[i] = false;
      }
   }
}

static bool oberon_joypad_rumble(unsigned pad,
      enum retro_rumble_effect effect, uint16_t strength)
{
   unsigned i;

   if (pad < OBERON_FIRST_PAD)
      return OBERON_INNER->set_rumble(pad, effect, strength);
   if (!oberon_get(pad))
      return false;

   /* goes out with the next request */
   i = pad - OBERON_FIRST_PAD;
   if (effect == RETRO_RUMBLE_STRONG)
      oberon_st.rumble[i][0] = (uint8_t)(strength >> 8);
   else if (effect == RETRO_RUMBLE_WEAK)
      oberon_st.rumble[i][1] = (uint8_t)(strength >> 8);
   return true;
}

input_device_driver_t oberon_joypad = {
   oberon_joypad_init,
   oberon_joypad_query_pad,
   oberon_joypad_destroy,
   oberon_joypad_button,
   oberon_joypad_state,
   oberon_joypad_get_buttons,
   oberon_joypad_axis,
   oberon_joypad_poll,
   oberon_joypad_rumble,
   NULL, /* set_rumble_gain */
   NULL, /* set_sensor_state */
   NULL, /* get_sensor_input */
   oberon_joypad_name,
   "oberon",
};

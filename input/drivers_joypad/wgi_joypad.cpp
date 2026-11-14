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

/* Windows.Gaming.Input pads for UWP. Some pads exist only here and not
 * in XInput, such as the virtual pads InputInjector creates on Xbox.
 * Buttons and axes are numbered as in xinput_joypad.c. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <string>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

#include <boolean.h>
#include <retro_inline.h>
#include <compat/strl.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../tasks/tasks_internal.h"
#include "../input_driver.h"
#include "../../verbosity.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Gaming::Input;

#define WGI_MAX_PADS    8
#define WGI_NUM_BUTTONS 10
#define WGI_NAME        "WGI Gamepad"

static const GamepadButtons wgi_button_map[WGI_NUM_BUTTONS] = {
   GamepadButtons::A,
   GamepadButtons::B,
   GamepadButtons::X,
   GamepadButtons::Y,
   GamepadButtons::LeftShoulder,
   GamepadButtons::RightShoulder,
   GamepadButtons::Menu,
   GamepadButtons::View,
   GamepadButtons::LeftThumbstick,
   GamepadButtons::RightThumbstick
};

typedef struct
{
   Gamepad          pad{ nullptr };
   GamepadReading   reading{};
   GamepadVibration vibration{};
   char             display_name[128];
   bool             connected;
} wgi_pad_t;

static wgi_pad_t         wgi_pads[WGI_MAX_PADS];
/* Set from the WinRT event threads, read by poll(). */
static std::atomic<bool> wgi_dirty{ true };
static bool              wgi_inited = false;
static event_token       wgi_added_token{};
static event_token       wgi_removed_token{};

static INLINE bool wgi_has(GamepadButtons word, GamepadButtons b)
{
   return (word & b) == b;
}

static int16_t wgi_to_axis(double v)
{
   if (v > 1.0)
      v = 1.0;
   else if (v < -1.0)
      v = -1.0;
   return (int16_t)(v * 32767.0);
}

static void wgi_clear_pad(wgi_pad_t *p)
{
   p->pad             = nullptr;
   p->reading         = GamepadReading{};
   p->vibration       = GamepadVibration{};
   p->display_name[0] = '\0';
   p->connected       = false;
}

static void wgi_fill_name(wgi_pad_t *p, unsigned *vid, unsigned *pid)
{
   strlcpy(p->display_name, WGI_NAME, sizeof(p->display_name));
   *vid = 0;
   *pid = 0;
   try
   {
      RawGameController raw = RawGameController::FromGameController(p->pad);
      if (raw)
      {
         std::string name = to_string(raw.DisplayName());
         if (!name.empty())
            strlcpy(p->display_name, name.c_str(), sizeof(p->display_name));
         *vid = raw.HardwareVendorId();
         *pid = raw.HardwareProductId();
      }
   }
   catch (...)
   {
   }
}

/* Pads keep their slot while connected. */
static void wgi_rescan(void)
{
   unsigned i, j;
   IVectorView<Gamepad> list{ nullptr };

   try
   {
      list = Gamepad::Gamepads();
   }
   catch (...)
   {
      return;
   }

   for (i = 0; i < WGI_MAX_PADS; i++)
   {
      uint32_t idx = 0;
      if (!wgi_pads[i].connected || list.IndexOf(wgi_pads[i].pad, idx))
         continue;
      input_autoconfigure_disconnect(i, wgi_pads[i].display_name);
      wgi_clear_pad(&wgi_pads[i]);
   }

   for (j = 0; j < list.Size(); j++)
   {
      unsigned vid, pid;
      wgi_pad_t *p  = NULL;
      bool known    = false;
      Gamepad gp    = list.GetAt(j);

      for (i = 0; i < WGI_MAX_PADS; i++)
      {
         if (wgi_pads[i].connected && wgi_pads[i].pad == gp)
         {
            known = true;
            break;
         }
         if (!wgi_pads[i].connected && !p)
            p = &wgi_pads[i];
      }

      if (known || !p)
         continue;

      p->pad       = gp;
      p->connected = true;
      wgi_fill_name(p, &vid, &pid);
      input_autoconfigure_connect(
            WGI_NAME,
            p->display_name,
            NULL,
            wgi_joypad.ident,
            (unsigned)(p - wgi_pads),
            vid,
            pid);
   }
}

static void *wgi_joypad_init(void *data)
{
   unsigned i;

   for (i = 0; i < WGI_MAX_PADS; i++)
      wgi_clear_pad(&wgi_pads[i]);

   try
   {
      wgi_added_token   = Gamepad::GamepadAdded(
            [](IInspectable const&, Gamepad const&)
            {
               wgi_dirty = true;
            });
      wgi_removed_token = Gamepad::GamepadRemoved(
            [](IInspectable const&, Gamepad const&)
            {
               wgi_dirty = true;
            });
   }
   catch (...)
   {
      RARCH_ERR("[WGI] Failed to register for gamepad events.\n");
      return NULL;
   }

   wgi_inited = true;
   wgi_dirty  = false;
   wgi_rescan();

   return (void*)-1;
}

static void wgi_joypad_destroy(void)
{
   unsigned i;

   if (wgi_inited)
   {
      try
      {
         Gamepad::GamepadAdded(wgi_added_token);
         Gamepad::GamepadRemoved(wgi_removed_token);
      }
      catch (...)
      {
      }
   }

   for (i = 0; i < WGI_MAX_PADS; i++)
   {
      if (wgi_pads[i].connected)
      {
         try
         {
            wgi_pads[i].pad.Vibration(GamepadVibration{});
         }
         catch (...)
         {
         }
      }
      wgi_clear_pad(&wgi_pads[i]);
   }

   wgi_inited = false;
   wgi_dirty  = true;
}

static INLINE wgi_pad_t *wgi_get(unsigned port)
{
   if (port >= WGI_MAX_PADS || !wgi_pads[port].connected)
      return NULL;
   return &wgi_pads[port];
}

static bool wgi_joypad_query_pad(unsigned port)
{
   return wgi_get(port) != NULL;
}

static const char *wgi_joypad_name(unsigned port)
{
   if (!wgi_get(port))
      return NULL;
   return WGI_NAME;
}

static int32_t wgi_button_state(const GamepadReading *r, uint16_t joykey)
{
   unsigned hat_dir = GET_HAT_DIR(joykey);

   if (hat_dir)
   {
      switch (hat_dir)
      {
         case HAT_UP_MASK:
            return wgi_has(r->Buttons, GamepadButtons::DPadUp);
         case HAT_DOWN_MASK:
            return wgi_has(r->Buttons, GamepadButtons::DPadDown);
         case HAT_LEFT_MASK:
            return wgi_has(r->Buttons, GamepadButtons::DPadLeft);
         case HAT_RIGHT_MASK:
            return wgi_has(r->Buttons, GamepadButtons::DPadRight);
         default:
            break;
      }
      return 0;
   }

   if (joykey < WGI_NUM_BUTTONS)
      return wgi_has(r->Buttons, wgi_button_map[joykey]);
   return 0;
}

static int16_t wgi_axis_state(const GamepadReading *r, uint32_t joyaxis)
{
   int16_t val = 0;
   int axis    = -1;
   bool is_neg = false;
   bool is_pos = false;

   /* triggers (axes 4,5) cannot be negative */
   if (AXIS_NEG_GET(joyaxis) <= 3)
   {
      axis   = AXIS_NEG_GET(joyaxis);
      is_neg = true;
   }
   else if (AXIS_POS_GET(joyaxis) <= 5)
   {
      axis   = AXIS_POS_GET(joyaxis);
      is_pos = true;
   }
   else
      return 0;

   switch (axis)
   {
      case 0:
         val = wgi_to_axis(r->LeftThumbstickX);
         break;
      case 1:
         val = wgi_to_axis(r->LeftThumbstickY);
         break;
      case 2:
         val = wgi_to_axis(r->RightThumbstickX);
         break;
      case 3:
         val = wgi_to_axis(r->RightThumbstickY);
         break;
      case 4:
         val = wgi_to_axis(r->LeftTrigger);
         break;
      case 5:
         val = wgi_to_axis(r->RightTrigger);
         break;
   }

   if (is_neg && val > 0)
      return 0;
   else if (is_pos && val < 0)
      return 0;
   return val;
}

static int32_t wgi_joypad_button(unsigned port, uint16_t joykey)
{
   wgi_pad_t *p = wgi_get(port);
   if (!p)
      return 0;
   return wgi_button_state(&p->reading, joykey);
}

static void wgi_joypad_get_buttons(unsigned port, input_bits_t *state)
{
   unsigned i;
   wgi_pad_t *p = wgi_get(port);

   BIT256_CLEAR_ALL_PTR(state);
   if (!p)
      return;
   for (i = 0; i < WGI_NUM_BUTTONS; i++)
      if (wgi_has(p->reading.Buttons, wgi_button_map[i]))
         BIT256_SET_PTR(state, i);
}

static int16_t wgi_joypad_axis(unsigned port, uint32_t joyaxis)
{
   wgi_pad_t *p = wgi_get(port);
   if (!p)
      return 0;
   return wgi_axis_state(&p->reading, joyaxis);
}

static int16_t wgi_joypad_state(
      rarch_joypad_info_t *joypad_info,
      const uint16_t *joykeys,
      const uint32_t *joyaxes,
      unsigned port)
{
   int i;
   int16_t ret  = 0;
   wgi_pad_t *p = wgi_get(joypad_info->joy_idx);

   if (!p)
      return 0;

   for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
   {
      const uint16_t joykey  = joykeys[i];
      const uint32_t joyaxis = joyaxes[i];
      if (     joykey != NO_BTN
            && wgi_button_state(&p->reading, joykey))
         ret |= (1 << i);
      else if (joyaxis != AXIS_NONE &&
            ((float)abs(wgi_axis_state(&p->reading, joyaxis))
             / 0x8000) > joypad_info->axis_threshold)
         ret |= (1 << i);
   }

   return ret;
}

static void wgi_joypad_poll(void)
{
   unsigned i;

   if (wgi_dirty.exchange(false))
      wgi_rescan();

   for (i = 0; i < WGI_MAX_PADS; i++)
   {
      if (!wgi_pads[i].connected)
         continue;
      try
      {
         wgi_pads[i].reading = wgi_pads[i].pad.GetCurrentReading();
      }
      catch (...)
      {
         wgi_pads[i].reading = GamepadReading{};
         wgi_dirty           = true;
      }
   }
}

static bool wgi_joypad_rumble(unsigned port,
      enum retro_rumble_effect effect, uint16_t strength)
{
   GamepadVibration prev;
   wgi_pad_t *p = wgi_get(port);

   if (!p)
      return false;

   prev = p->vibration;

   /* Low frequency (left) motor is the "strong" one, as in xinput. */
   if (effect == RETRO_RUMBLE_STRONG)
      p->vibration.LeftMotor  = strength / 65535.0;
   else if (effect == RETRO_RUMBLE_WEAK)
      p->vibration.RightMotor = strength / 65535.0;

   if (     (p->vibration.LeftMotor  == prev.LeftMotor)
         && (p->vibration.RightMotor == prev.RightMotor))
      return true;

   try
   {
      p->pad.Vibration(p->vibration);
   }
   catch (...)
   {
      return false;
   }
   return true;
}

input_device_driver_t wgi_joypad = {
   wgi_joypad_init,
   wgi_joypad_query_pad,
   wgi_joypad_destroy,
   wgi_joypad_button,
   wgi_joypad_state,
   wgi_joypad_get_buttons,
   wgi_joypad_axis,
   wgi_joypad_poll,
   wgi_joypad_rumble,
   NULL, /* set_rumble_gain */
   NULL, /* set_sensor_state */
   NULL, /* get_sensor_input */
   wgi_joypad_name,
   "wgi",
};

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
 * Pads that are a Gamepad are numbered as in xinput_joypad.c; any other
 * RawGameController is read as plain buttons, hats and axes. */

#include <stdint.h>
#include <stdio.h>
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
#include "../../runloop.h"
#include "../../verbosity.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Gaming::Input;

#define WGI_MAX_PADS        8
#define WGI_NUM_BUTTONS     10
#define WGI_RAW_MAX_BUTTONS 64
#define WGI_RAW_MAX_SWITCH  4
#define WGI_RAW_MAX_AXES    16
#define WGI_NAME            "WGI Gamepad"

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

typedef struct wgi_pad
{
   RawGameController           raw{ nullptr };
   Gamepad                     pad{ nullptr };
   GamepadReading              reading{};
   GamepadVibration            vibration{};
   bool                        raw_buttons[WGI_RAW_MAX_BUTTONS];
   GameControllerSwitchPosition raw_switches[WGI_RAW_MAX_SWITCH];
   double                      raw_axes[WGI_RAW_MAX_AXES];
   unsigned                    num_buttons;
   unsigned                    num_switches;
   unsigned                    num_axes;
   char                        display_name[128];
   bool                        connected;
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
   unsigned i;

   p->raw             = nullptr;
   p->pad             = nullptr;
   p->reading         = GamepadReading{};
   p->vibration       = GamepadVibration{};
   memset(p->raw_buttons, 0, sizeof(p->raw_buttons));
   memset(p->raw_axes,    0, sizeof(p->raw_axes));
   for (i = 0; i < WGI_RAW_MAX_SWITCH; i++)
      p->raw_switches[i] = GameControllerSwitchPosition::Center;
   p->num_buttons     = 0;
   p->num_switches    = 0;
   p->num_axes        = 0;
   p->display_name[0] = '\0';
   p->connected       = false;
}

static void wgi_attach(wgi_pad_t *p, RawGameController const &raw)
{
   unsigned vid = 0, pid = 0;

   p->raw       = raw;
   p->connected = true;
   strlcpy(p->display_name, WGI_NAME, sizeof(p->display_name));

   try
   {
      p->pad = Gamepad::FromGameController(raw);
   }
   catch (...)
   {
      p->pad = nullptr;
   }

   try
   {
      std::string name = to_string(raw.DisplayName());
      if (!name.empty())
         strlcpy(p->display_name, name.c_str(), sizeof(p->display_name));
      vid             = raw.HardwareVendorId();
      pid             = raw.HardwareProductId();
      p->num_buttons  = (unsigned)raw.ButtonCount();
      p->num_switches = (unsigned)raw.SwitchCount();
      p->num_axes     = (unsigned)raw.AxisCount();
   }
   catch (...)
   {
   }

   RARCH_LOG("[WGI] Pad %u: \"%s\" (%04x:%04x) %s, %u buttons, %u switches, %u axes.\n",
         (unsigned)(p - wgi_pads), p->display_name, vid, pid,
         p->pad ? "gamepad" : "raw",
         p->num_buttons, p->num_switches, p->num_axes);

   /* A Gamepad is found by WGI_NAME for the built-in profile,
    * anything else by its own name. */
   input_autoconfigure_connect(
         p->pad ? WGI_NAME : p->display_name,
         p->display_name,
         NULL,
         wgi_joypad.ident,
         (unsigned)(p - wgi_pads),
         vid,
         pid);
}

/* Pads keep their slot while connected. */
static void wgi_rescan(void)
{
   char msg[128];
   unsigned i, j;
   unsigned total    = 0;
   unsigned gamepads = 0;
   IVectorView<RawGameController> list{ nullptr };

   try
   {
      list = RawGameController::RawGameControllers();
   }
   catch (...)
   {
      RARCH_ERR("[WGI] RawGameControllers() failed.\n");
      return;
   }

   for (i = 0; i < WGI_MAX_PADS; i++)
   {
      uint32_t idx = 0;
      if (!wgi_pads[i].connected || list.IndexOf(wgi_pads[i].raw, idx))
         continue;
      input_autoconfigure_disconnect(i, wgi_pads[i].display_name);
      wgi_clear_pad(&wgi_pads[i]);
   }

   for (j = 0; j < list.Size(); j++)
   {
      wgi_pad_t *p          = NULL;
      bool known            = false;
      RawGameController raw = list.GetAt(j);

      for (i = 0; i < WGI_MAX_PADS; i++)
      {
         if (wgi_pads[i].connected && wgi_pads[i].raw == raw)
         {
            known = true;
            break;
         }
         if (!wgi_pads[i].connected && !p)
            p = &wgi_pads[i];
      }

      if (!known && p)
         wgi_attach(p, raw);
   }

   for (i = 0; i < WGI_MAX_PADS; i++)
   {
      if (!wgi_pads[i].connected)
         continue;
      total++;
      if (wgi_pads[i].pad)
         gamepads++;
   }

   snprintf(msg, sizeof(msg), "WGI: %u controller(s), %u gamepad(s), %u listed.",
         total, gamepads, (unsigned)list.Size());
   RARCH_LOG("[WGI] %s\n", msg);
   runloop_msg_queue_push(msg, strlen(msg), 1, 240, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

static void *wgi_joypad_init(void *data)
{
   unsigned i;

   for (i = 0; i < WGI_MAX_PADS; i++)
      wgi_clear_pad(&wgi_pads[i]);

   try
   {
      wgi_added_token   = RawGameController::RawGameControllerAdded(
            [](IInspectable const&, RawGameController const&)
            {
               wgi_dirty = true;
            });
      wgi_removed_token = RawGameController::RawGameControllerRemoved(
            [](IInspectable const&, RawGameController const&)
            {
               wgi_dirty = true;
            });
   }
   catch (...)
   {
      RARCH_ERR("[WGI] Failed to register for controller events.\n");
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
         RawGameController::RawGameControllerAdded(wgi_added_token);
         RawGameController::RawGameControllerRemoved(wgi_removed_token);
      }
      catch (...)
      {
      }
   }

   for (i = 0; i < WGI_MAX_PADS; i++)
   {
      if (wgi_pads[i].connected && wgi_pads[i].pad)
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
   wgi_pad_t *p = wgi_get(port);
   if (!p)
      return NULL;
   return p->pad ? WGI_NAME : p->display_name;
}

static bool wgi_raw_hat(const wgi_pad_t *p, unsigned hat, unsigned dir)
{
   GameControllerSwitchPosition s;

   if (hat >= p->num_switches || hat >= WGI_RAW_MAX_SWITCH)
      return false;
   s = p->raw_switches[hat];

   switch (dir)
   {
      case HAT_UP_MASK:
         return s == GameControllerSwitchPosition::Up
             || s == GameControllerSwitchPosition::UpLeft
             || s == GameControllerSwitchPosition::UpRight;
      case HAT_DOWN_MASK:
         return s == GameControllerSwitchPosition::Down
             || s == GameControllerSwitchPosition::DownLeft
             || s == GameControllerSwitchPosition::DownRight;
      case HAT_LEFT_MASK:
         return s == GameControllerSwitchPosition::Left
             || s == GameControllerSwitchPosition::UpLeft
             || s == GameControllerSwitchPosition::DownLeft;
      case HAT_RIGHT_MASK:
         return s == GameControllerSwitchPosition::Right
             || s == GameControllerSwitchPosition::UpRight
             || s == GameControllerSwitchPosition::DownRight;
      default:
         break;
   }
   return false;
}

static int32_t wgi_button_state(const wgi_pad_t *p, uint16_t joykey)
{
   unsigned hat_dir = GET_HAT_DIR(joykey);

   if (!p->pad)
   {
      if (hat_dir)
         return wgi_raw_hat(p, GET_HAT(joykey), hat_dir);
      if (joykey < p->num_buttons && joykey < WGI_RAW_MAX_BUTTONS)
         return p->raw_buttons[joykey];
      return 0;
   }

   if (hat_dir)
   {
      switch (hat_dir)
      {
         case HAT_UP_MASK:
            return wgi_has(p->reading.Buttons, GamepadButtons::DPadUp);
         case HAT_DOWN_MASK:
            return wgi_has(p->reading.Buttons, GamepadButtons::DPadDown);
         case HAT_LEFT_MASK:
            return wgi_has(p->reading.Buttons, GamepadButtons::DPadLeft);
         case HAT_RIGHT_MASK:
            return wgi_has(p->reading.Buttons, GamepadButtons::DPadRight);
         default:
            break;
      }
      return 0;
   }

   if (joykey < WGI_NUM_BUTTONS)
      return wgi_has(p->reading.Buttons, wgi_button_map[joykey]);
   return 0;
}

static int16_t wgi_axis_state(const wgi_pad_t *p, uint32_t joyaxis)
{
   int16_t val = 0;
   int axis    = -1;
   bool is_neg = false;
   bool is_pos = false;

   if (AXIS_NEG_GET(joyaxis) < WGI_RAW_MAX_AXES)
   {
      axis   = AXIS_NEG_GET(joyaxis);
      is_neg = true;
   }
   else if (AXIS_POS_GET(joyaxis) < WGI_RAW_MAX_AXES)
   {
      axis   = AXIS_POS_GET(joyaxis);
      is_pos = true;
   }
   else
      return 0;

   if (!p->pad)
   {
      /* raw axes go from 0 to 1, centered at 0.5 */
      if ((unsigned)axis >= p->num_axes)
         return 0;
      val = wgi_to_axis(p->raw_axes[axis] * 2.0 - 1.0);
   }
   else
   {
      /* triggers (axes 4,5) cannot be negative */
      if (is_neg && axis > 3)
         return 0;
      switch (axis)
      {
         case 0:
            val = wgi_to_axis(p->reading.LeftThumbstickX);
            break;
         case 1:
            val = wgi_to_axis(p->reading.LeftThumbstickY);
            break;
         case 2:
            val = wgi_to_axis(p->reading.RightThumbstickX);
            break;
         case 3:
            val = wgi_to_axis(p->reading.RightThumbstickY);
            break;
         case 4:
            val = wgi_to_axis(p->reading.LeftTrigger);
            break;
         case 5:
            val = wgi_to_axis(p->reading.RightTrigger);
            break;
         default:
            return 0;
      }
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
   return wgi_button_state(p, joykey);
}

static void wgi_joypad_get_buttons(unsigned port, input_bits_t *state)
{
   unsigned i;
   wgi_pad_t *p = wgi_get(port);

   BIT256_CLEAR_ALL_PTR(state);
   if (!p)
      return;
   if (!p->pad)
   {
      for (i = 0; i < p->num_buttons && i < WGI_RAW_MAX_BUTTONS; i++)
         if (p->raw_buttons[i])
            BIT256_SET_PTR(state, i);
      return;
   }
   for (i = 0; i < WGI_NUM_BUTTONS; i++)
      if (wgi_has(p->reading.Buttons, wgi_button_map[i]))
         BIT256_SET_PTR(state, i);
}

static int16_t wgi_joypad_axis(unsigned port, uint32_t joyaxis)
{
   wgi_pad_t *p = wgi_get(port);
   if (!p)
      return 0;
   return wgi_axis_state(p, joyaxis);
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
            && wgi_button_state(p, joykey))
         ret |= (1 << i);
      else if (joyaxis != AXIS_NONE &&
            ((float)abs(wgi_axis_state(p, joyaxis))
             / 0x8000) > joypad_info->axis_threshold)
         ret |= (1 << i);
   }

   return ret;
}

static void wgi_poll_raw(wgi_pad_t *p)
{
   if (     p->num_buttons  > WGI_RAW_MAX_BUTTONS
         || p->num_switches > WGI_RAW_MAX_SWITCH
         || p->num_axes     > WGI_RAW_MAX_AXES)
      return;
   p->raw.GetCurrentReading(
         array_view<bool>(p->raw_buttons, p->raw_buttons + p->num_buttons),
         array_view<GameControllerSwitchPosition>(p->raw_switches,
            p->raw_switches + p->num_switches),
         array_view<double>(p->raw_axes, p->raw_axes + p->num_axes));
}

static void wgi_joypad_poll(void)
{
   unsigned i;

   if (wgi_dirty.exchange(false))
      wgi_rescan();

   for (i = 0; i < WGI_MAX_PADS; i++)
   {
      wgi_pad_t *p = &wgi_pads[i];
      if (!p->connected)
         continue;
      try
      {
         if (p->pad)
            p->reading = p->pad.GetCurrentReading();
         else
            wgi_poll_raw(p);
      }
      catch (...)
      {
         p->reading = GamepadReading{};
         memset(p->raw_buttons, 0, sizeof(p->raw_buttons));
         wgi_dirty  = true;
      }
   }
}

static bool wgi_joypad_rumble(unsigned port,
      enum retro_rumble_effect effect, uint16_t strength)
{
   GamepadVibration prev;
   wgi_pad_t *p = wgi_get(port);

   if (!p || !p->pad)
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

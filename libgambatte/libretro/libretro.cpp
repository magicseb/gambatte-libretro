#include "libretro.h"
#include "resamplerinfo.h"

#include <gambatte.h>

#include <assert.h>
#include <stdio.h>

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static gambatte::GB gb;

namespace input
{
   struct map { unsigned snes; unsigned gb; };
   static const map btn_map[] = {
      { RETRO_DEVICE_ID_JOYPAD_A, gambatte::InputGetter::A },
      { RETRO_DEVICE_ID_JOYPAD_B, gambatte::InputGetter::B },
      { RETRO_DEVICE_ID_JOYPAD_SELECT, gambatte::InputGetter::SELECT },
      { RETRO_DEVICE_ID_JOYPAD_START, gambatte::InputGetter::START },
      { RETRO_DEVICE_ID_JOYPAD_RIGHT, gambatte::InputGetter::RIGHT },
      { RETRO_DEVICE_ID_JOYPAD_LEFT, gambatte::InputGetter::LEFT },
      { RETRO_DEVICE_ID_JOYPAD_UP, gambatte::InputGetter::UP },
      { RETRO_DEVICE_ID_JOYPAD_DOWN, gambatte::InputGetter::DOWN },
   };
}

class SNESInput : public gambatte::InputGetter
{
   public:
      unsigned operator()()
      {
         input_poll_cb();
         unsigned res = 0;
         for (unsigned i = 0; i < sizeof(input::btn_map) / sizeof(input::map); i++)
            res |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, input::btn_map[i].snes) ? input::btn_map[i].gb : 0;
         return res;
      }
} static gb_input;

static Resampler *resampler;


void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "gambatte";
   info->library_version = "v0.5.0";
   info->need_fullpath = false;
   info->block_extract = false;
   info->valid_extensions = "gb|gbc|dmg|zip|GB|GBC|DMG|ZIP";
}

static bool can_dupe = false;
static struct retro_system_timing g_timing;

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   retro_game_geometry geom = { 160, 144, 160, 144 };
   info->geometry = geom;
   info->timing   = g_timing;
}

void retro_init()
{
   // Using uint_least32_t in an audio interface expecting you to cast to short*? :( Weird stuff.
   assert(sizeof(gambatte::uint_least32_t) == sizeof(uint32_t));
   gb.setInputGetter(&gb_input);

   double fps = 4194304.0 / 70224.0;
   double sample_rate = fps * 35112;

   resampler = ResamplerInfo::get(ResamplerInfo::num() - 1).create(sample_rate, 32000.0, 2 * 2064);

   if (environ_cb)
   {
      g_timing.fps = fps;

      unsigned long mul, div;
      resampler->exactRatio(mul, div);

      g_timing.sample_rate = sample_rate * mul / div;

      environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe);
      if (can_dupe)
         fprintf(stderr, "[Gambatte]: Will dupe frames with NULL!\n");
   }
}

void retro_deinit()
{
   delete resampler;
}

void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_set_controller_port_device(unsigned, unsigned) {}

void retro_reset() { gb.reset(); }

static size_t serialize_size = 0;
size_t retro_serialize_size()
{
   return gb.stateSize();
}

bool retro_serialize(void *data, size_t size)
{
   if (serialize_size == 0)
      serialize_size = retro_serialize_size();

   if (size != serialize_size)
      return false;

   gb.saveState(data);
   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   if (serialize_size == 0)
      serialize_size = retro_serialize_size();

   if (size != serialize_size)
      return false;

   gb.loadState(data);
   return true;
}

void retro_cheat_reset() {}
void retro_cheat_set(unsigned, bool, const char *) {}

bool retro_load_game(const struct retro_game_info *info)
{
   return !gb.load(info->data, info->size);
}

bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

void retro_unload_game() {}

unsigned retro_get_region() { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return gb.savedata_ptr();
   if (id == RETRO_MEMORY_RTC)
      return gb.rtcdata_ptr();

   return 0;
}

size_t retro_get_memory_size(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return gb.savedata_size();
   if (id == RETRO_MEMORY_RTC)
      return gb.rtcdata_size();

   return 0;
}

static void convert_frame(uint16_t *output, const uint32_t *input)
{
   for (unsigned y = 0; y < 144; y++)
   {
      const uint32_t *src = input + y * 256;
      uint16_t *dst = output + y * 256;

      for (unsigned x = 0; x < 160; x++)
      {
         unsigned color = src[x];
         unsigned out_color = 0;

         // ARGB8888 => XRGB555. Should output 32-bit directly if libretro gets ARGB support later on.
         out_color |= (color & 0xf80000) >> (3 + 6);
         out_color |= (color & 0x00f800) >> (3 + 3);
         out_color |= (color & 0x0000f8) >> (3 + 0);

         dst[x] = out_color;
      }
   }
}

static void output_audio(const int16_t *samples, unsigned frames)
{
   if (!frames)
      return;

   int16_t output[2 * 2064];
   std::size_t len = resampler->resample(output, samples, frames);

   if (len)
      audio_batch_cb(output, len);
}

void retro_run()
{
   static uint64_t samples_count = 0;
   static uint64_t frames_count = 0;
   static uint16_t output_video[256 * 144];

   input_poll_cb();

   uint64_t expected_frames = samples_count / 35112;
   if (frames_count < expected_frames) // Detect frame dupes.
   {
      video_cb(can_dupe ? 0 : output_video, 160, 144, 512);
      frames_count++;
      return;
   }

   union
   {
      uint32_t u32[2064 + 2064];
      int16_t i16[2 * (2064 + 2064)];
   } sound_buf;
   unsigned samples = 2064;

   uint32_t video_buf[256 * 144];
   while (gb.runFor(video_buf, 256, sound_buf.u32, samples) == -1)
   {
      output_audio(sound_buf.i16, samples);
      samples_count += samples;
      samples = 2064;
   }

   samples_count += samples;
   output_audio(sound_buf.i16, samples);

   convert_frame(output_video, video_buf);
   video_cb(output_video, 160, 144, 512);
   frames_count++;
}

unsigned retro_api_version() { return RETRO_API_VERSION; }


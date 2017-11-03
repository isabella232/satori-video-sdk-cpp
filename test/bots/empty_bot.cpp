#include <satorivideo/video_bot.h>
#include <iostream>
#include "../../src/cbor_tools.h"

#define LOGURU_WITH_STREAMS 1
#include <loguru/loguru.hpp>

using namespace satori::video;

namespace empty_bot {

void process_image(bot_context &context, const image_frame & /*frame*/) {
  LOG_S(INFO) << "got frame " << context.frame_metadata->width << "x"
              << context.frame_metadata->height;
  cbor_item_t *msg = cbor_new_indefinite_map();
  cbor_map_add(
      msg, {cbor_move(cbor_build_string("msg")), cbor_move(cbor_build_string("hello"))});
  bot_message(context, bot_message_kind::ANALYSIS, cbor_move(msg));
}

}  // namespace empty_bot

int main(int argc, char *argv[]) {
  bot_register(bot_descriptor{image_pixel_format::BGR, &empty_bot::process_image});
  return bot_main(argc, argv);
}
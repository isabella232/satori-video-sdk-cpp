#include <assert.h>
#include <librtmvideo/cbor_tools.h>
#include <librtmvideo/video_bot.h>
#include <iostream>

namespace test_bot {
struct State {
  int magic_number;
};
cbor_item_t *build_message(const std::string &text) {
  cbor_item_t *message = cbor_new_indefinite_map();
  cbor_map_add(message, {.key = cbor_move(cbor_build_string("message")),
                         .value = cbor_move(cbor_build_string(text.c_str()))});
  return cbor_move(message);
}
void process_image(bot_context &context, const uint8_t *image, uint16_t width,
                   uint16_t height, uint16_t linesize) {
  assert(context.instance_data != nullptr);  // Make sure initialization passed
  std::cout << "got frame " << width << "x" << height << "\n";
  rtm_video_bot_message(context, bot_message_kind::ANALYSIS,
                        cbor_move(build_message("Hello from bot 1")));
  rtm_video_bot_message(context, bot_message_kind::DEBUG,
                        cbor_move(build_message("Hello from bot 2")));
}
cbor_item_t *process_command(bot_context &ctx, cbor_item_t *config) {
  if (cbor::map_has_str_value(config, "action", "configure")) {
    assert(ctx.instance_data == nullptr);  // Make sure is has initialized once
    State *state = new State;
    std::cout << "bot is initializing, libraries are ok" << '\n';
    std::string p =
        cbor::map_get_str(cbor::map_get(config, "body"), "myparam", "");
    assert(p.compare("myvalue") == 0);  // Make sure parameter passed
    ctx.instance_data = state;
  }
  return nullptr;
}
}  // namespace test_bot

int main(int argc, char *argv[]) {
  rtm_video_bot_register(bot_descriptor{640, 480, image_pixel_format::BGR,
                                        &test_bot::process_image,
                                        &test_bot::process_command});
  return rtm_video_bot_main(argc, argv);
}

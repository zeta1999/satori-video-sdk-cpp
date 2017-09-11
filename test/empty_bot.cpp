#include <assert.h>
#include <librtmvideo/cbor_tools.h>
#include <librtmvideo/video_bot.h>
#include <iostream>

namespace empty_bot {

void process_image(bot_context &context, const image_frame &frame) {
  image_metadata m;
  rtm_video_bot_get_metadata(m, context);
  std::cout << "got frame " << m.width << "x" << m.height << "\n";
}
cbor_item_t *process_command(bot_context &ctx, cbor_item_t *config) {
  return nullptr;
}

}  // namespace empty_bot

int main(int argc, char *argv[]) {
  rtm_video_bot_register(bot_descriptor{640, 480, image_pixel_format::BGR,
                                        &empty_bot::process_image,
                                        &empty_bot::process_command});
  return rtm_video_bot_main(argc, argv);
}

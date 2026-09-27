#include "listener.h"

#include "frame.h"

namespace atlantic_v5 {

namespace {
constexpr uint64_t TXN_MASK = 0xFFULL << 24;
constexpr uint64_t TXN_READ = 0x64ULL << 24;
}  // namespace

void Listener::push_byte(uint8_t byte, uint32_t t_us) {
  if (assembler_.push(byte, t_us))
    handle_frame(t_us);
}

void Listener::tick(uint32_t t_us) {
  if (assembler_.tick(t_us))
    handle_frame(t_us);
}

void Listener::handle_frame(uint32_t t_us) {
  Frame f(Channel::BUS, assembler_.frame(), assembler_.frame_len());
  if (!f.crc_valid())
    return;
  if (f.has_payload() && (f.header_key() & TXN_MASK) == TXN_READ)
    last_main_us_ = t_us;
  if (frame_sink_ != nullptr)
    frame_sink_(frame_ctx_, f, t_us);
  if (sink_ != nullptr)
    decoder_.decode(f, sink_, ctx_);
}

}  // namespace atlantic_v5

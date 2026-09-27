#include "replay_lib.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "assembler.h"
#include "catalog.h"
#include "decoder.h"
#include "frame.h"

namespace replay {

namespace {

std::vector<uint8_t> parse_hex(const std::string &hex) {
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return out;
}

std::string to_hex_upper(const uint8_t *data, size_t len) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += kDigits[(data[i] >> 4) & 0xF];
    out += kDigits[data[i] & 0xF];
  }
  return out;
}

struct Collector {
  std::vector<atlantic_v5::DecodedValue> values;
};

void collect(void *ctx, const atlantic_v5::DecodedValue &v) { static_cast<Collector *>(ctx)->values.push_back(v); }

void append_value_json(std::string &out, const atlantic_v5::DecodedValue &v) {
  out += "{\"id\":\"";
  out += atlantic_v5::entity_name(static_cast<atlantic_v5::EntityId>(v.id));
  out += "\",";
  char buf[64];
  switch (v.kind) {
    case atlantic_v5::DecodedValue::Kind::FLOAT:
      std::snprintf(buf, sizeof(buf), "\"f\":%.2f", static_cast<double>(v.f));
      out += buf;
      break;
    case atlantic_v5::DecodedValue::Kind::BOOL:
      out += "\"b\":";
      out += (v.b ? "true" : "false");
      break;
    case atlantic_v5::DecodedValue::Kind::UINT:
      std::snprintf(buf, sizeof(buf), "\"u\":%u", v.u);
      out += buf;
      break;
    case atlantic_v5::DecodedValue::Kind::TEXT:
      out += "\"s\":\"";
      out += v.text;
      out += "\"";
      break;
  }
  out += "}";
}

// One Appendix A JSON line for a single delivered, framed byte run. "values"
// is only present when the CRC is valid (plan 2.6: never decode otherwise).
std::string format_line(uint32_t t_us, const std::string &channel_name, atlantic_v5::Channel ch, const uint8_t *data,
                         uint8_t len, atlantic_v5::Decoder &decoder) {
  atlantic_v5::Frame f(ch, data, len);
  bool crc_ok = f.crc_valid();

  std::string out = "{\"t_us\":" + std::to_string(t_us) + ",\"ch\":\"" + channel_name + "\",\"hex\":\"" +
                     to_hex_upper(data, len) + "\",\"key\":\"" + to_hex_upper(data, atlantic_v5::HEADER_LEN) +
                     "\",\"crc\":" + (crc_ok ? "true" : "false");

  if (crc_ok) {
    Collector c;
    decoder.decode(f, collect, &c);
    out += ",\"values\":[";
    for (size_t i = 0; i < c.values.size(); i++) {
      if (i > 0)
        out += ",";
      append_value_json(out, c.values[i]);
    }
    out += "]";
  }
  out += "}";
  return out;
}

}  // namespace

std::vector<Row> load_capture(const std::string &path, const ChannelOf &channel_of) {
  std::vector<Row> rows;
  std::ifstream in(path);
  if (!in.is_open())
    return rows;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::stringstream ss(line);
    std::string ts, channel_name, hex;
    std::getline(ss, ts, ',');
    std::getline(ss, channel_name, ',');
    std::getline(ss, hex, ',');
    rows.push_back({static_cast<uint32_t>(std::stoul(ts)), channel_name, channel_of(channel_name), parse_hex(hex)});
  }
  return rows;
}

std::vector<std::string> replay_dual_bus(const std::vector<Row> &rows) {
  atlantic_v5::FrameAssembler hmi(atlantic_v5::Channel::HMI, /*dual_bus=*/true);
  atlantic_v5::FrameAssembler main_asm(atlantic_v5::Channel::MAIN, /*dual_bus=*/true);
  atlantic_v5::Decoder decoder;
  std::vector<std::string> lines;
  const Row *last_hmi_row = nullptr;
  const Row *last_main_row = nullptr;

  for (const auto &row : rows) {
    bool is_hmi = row.channel == atlantic_v5::Channel::HMI;
    atlantic_v5::FrameAssembler &a = is_hmi ? hmi : main_asm;
    (is_hmi ? last_hmi_row : last_main_row) = &row;
    if (a.tick(row.t_us))
      lines.push_back(format_line(row.t_us, row.channel_name, row.channel, a.frame(), a.frame_len(), decoder));
    for (uint8_t b : row.bytes) {
      if (a.push(b, row.t_us))
        lines.push_back(format_line(row.t_us, row.channel_name, row.channel, a.frame(), a.frame_len(), decoder));
    }
  }

  // Flush any trailing partial frame on either side past the silence backstop
  // (plan 2.5.1 #6), same as replay_single_bus does - a capture can end
  // mid-frame on either channel, not just on a single-wire one.
  if (last_hmi_row != nullptr) {
    uint32_t t_us = last_hmi_row->t_us + atlantic_v5::FrameAssembler::DEFAULT_SILENCE_US + 1;
    if (hmi.tick(t_us))
      lines.push_back(format_line(t_us, last_hmi_row->channel_name, last_hmi_row->channel, hmi.frame(),
                                   hmi.frame_len(), decoder));
  }
  if (last_main_row != nullptr) {
    uint32_t t_us = last_main_row->t_us + atlantic_v5::FrameAssembler::DEFAULT_SILENCE_US + 1;
    if (main_asm.tick(t_us))
      lines.push_back(format_line(t_us, last_main_row->channel_name, last_main_row->channel, main_asm.frame(),
                                   main_asm.frame_len(), decoder));
  }
  return lines;
}

std::vector<std::string> replay_single_bus(const std::vector<Row> &rows) {
  atlantic_v5::FrameAssembler bus(atlantic_v5::Channel::BUS, /*dual_bus=*/false);
  atlantic_v5::Decoder decoder;
  std::vector<std::string> lines;

  for (const auto &row : rows) {
    if (bus.tick(row.t_us))
      lines.push_back(format_line(row.t_us, row.channel_name, row.channel, bus.frame(), bus.frame_len(), decoder));
    for (uint8_t b : row.bytes) {
      if (bus.push(b, row.t_us))
        lines.push_back(format_line(row.t_us, row.channel_name, row.channel, bus.frame(), bus.frame_len(), decoder));
    }
  }
  if (!rows.empty() && bus.tick(rows.back().t_us + atlantic_v5::FrameAssembler::DEFAULT_SILENCE_US + 1))
    lines.push_back(format_line(rows.back().t_us, rows.back().channel_name, rows.back().channel, bus.frame(),
                                 bus.frame_len(), decoder));
  return lines;
}

}  // namespace replay

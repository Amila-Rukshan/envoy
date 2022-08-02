#include "source/common/websocket/codec.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <algorithm>

#include "source/common/buffer/buffer_impl.h"

#include "absl/container/fixed_array.h"

namespace Envoy {
namespace WebSocket {

bool Decoder::decode(Buffer::Instance& input, std::vector<Frame>& output) {
  decoding_error_ = false;
  output_ = &output;
  inspect(input);
  output_ = nullptr;
  if (decoding_error_) {
    return false;
  }
  input.drain(input.length());
  return true;
}

bool Decoder::frameStart(uint8_t flags_and_opcode) {
  // Validate opcode (last 4 bits)
  uint8_t opcode = flags_and_opcode & 0x0f;
  if (std::find(frame_opcodes.begin(), frame_opcodes.end(), opcode) != frame_opcodes.end()) {
    frame_.flags_and_opcode_ = flags_and_opcode;
    return true;
  }
  decoding_error_ = true;
  return false;
}

void Decoder::frameMaskFlag(uint8_t mask_and_length) {
  // Set masked flag
  if (mask_and_length & 0x80) {
    frame_.is_masked_ = true;
    masking_key_length_ = 4;
  }
  // Set length (0 to 125) or length flag (126 or 127)
  length_ = mask_and_length & 0x7F;
}

void Decoder::frameMaskingKey() { frame_.masking_key_ = masking_key_; }

void Decoder::frameDataStart() {
  frame_.payload_length_ = length_;
  frame_.payload_ = std::make_unique<Buffer::OwnedImpl>();
}

void Decoder::frameData(uint8_t* mem, uint64_t length) { frame_.payload_->add(mem, length); }

void Decoder::frameDataEnd() {
  output_->push_back(std::move(frame_));
  frame_.flags_and_opcode_ = 0;
  frame_.payload_length_ = 0;
  frame_.payload_ = nullptr;
  frame_.is_masked_ = false;
}

uint64_t FrameInspector::inspect(const Buffer::Instance& data) {
  uint64_t delta = 0;
  for (const Buffer::RawSlice& slice : data.getRawSlices()) {
    uint8_t* mem = reinterpret_cast<uint8_t*>(slice.mem_);
    for (uint64_t j = 0; j < slice.len_;) {
      uint8_t c = *mem;
      switch (state_) {
      case State::FhFlagsAndOpcode:
        if (!frameStart(c)) {
          return delta;
        }
        count_ += 1;
        delta += 1;
        state_ = State::FhMaskFlagAndLength;
        mem++;
        j++;
        break;
      case State::FhMaskFlagAndLength:
        frameMaskFlag(c);
        if (length_ == 0x7e) {
          length_of_extended_length_ = 2;
          length_ = 0;
          state_ = State::FhExtendedLength;
        } else if (length_ == 0x7f) {
          length_of_extended_length_ = 8;
          length_ = 0;
          state_ = State::FhExtendedLength;
        } else if (masking_key_length_ > 0) {
          state_ = State::FhMaskingKey;
        } else {
          frameDataStart();
          if (length_ == 0) {
            frameDataEnd();
            state_ = State::FhFlagsAndOpcode;
          } else {
            state_ = State::Payload;
          }
        }
        mem++;
        j++;
        break;
      case State::FhExtendedLength:
        if (length_of_extended_length_ == 1) {
          length_ |= static_cast<uint32_t>(c);
        } else {
          length_ |= static_cast<uint32_t>(c) << 8 * (length_of_extended_length_ - 1);
        }
        length_of_extended_length_--;
        if (length_of_extended_length_ == 0) {
          if (masking_key_length_ > 0) {
            state_ = State::FhMaskingKey;
          } else {
            frameDataStart();
            if (length_ == 0) {
              frameDataEnd();
              state_ = State::FhFlagsAndOpcode;
            } else {
              state_ = State::Payload;
            }
          }
        }
        mem++;
        j++;
      case State::FhMaskingKey:
        if (masking_key_length_ == 1) {
          masking_key_ |= static_cast<uint32_t>(c);
        } else {
          masking_key_ |= static_cast<uint32_t>(c) << 8 * (masking_key_length_ - 1);
        }
        masking_key_length_--;
        if (masking_key_length_ == 0) {
          frameMaskingKey();
          frameDataStart();
          if (length_ == 0) {
            frameDataEnd();
            state_ = State::FhFlagsAndOpcode;
          } else {
            state_ = State::Payload;
          }
        }
        mem++;
        j++;
      case State::Payload:
        uint64_t remain_in_buffer = slice.len_ - j;
        if (remain_in_buffer <= length_) {
          frameData(mem, remain_in_buffer);
          mem += remain_in_buffer;
          j += remain_in_buffer;
          length_ -= remain_in_buffer;
        } else {
          frameData(mem, length_);
          mem += length_;
          j += length_;
          length_ = 0;
        }
        if (length_ == 0) {
          frameDataEnd();
          state_ = State::FhFlagsAndOpcode;
        }
        break;
      }
    }
  }
  return delta;
}

} // namespace WebSocket
} // namespace Envoy

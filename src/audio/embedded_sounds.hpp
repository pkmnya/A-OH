#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aoh {

struct EmbeddedSoundView {
    const char* uri;
    const uint8_t* data;
    size_t size;
};

const EmbeddedSoundView* find_embedded_sound(const std::string& uri);
std::vector<std::string> list_embedded_sound_uris();

} // namespace aoh

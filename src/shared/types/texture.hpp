#pragma once

#include <cstdint>
#include <vector>

#include "msgpack.hpp"

#include "utils/tostring.hpp"

namespace fr {

struct Texture {
    enum Kind {
        NONE       = 0,
        PROCEDURAL = 1,
        IMAGE      = 2
    };

    explicit Texture(uint64_t id);

    explicit Texture(uint64_t id, const std::string& code);

    explicit Texture(uint64_t id, int16_t width, int16_t height, const float* data);

    // FOR MSGPACK ONLY!
    explicit Texture();

    /// Resource ID of the texture.
    uint64_t id;

    /// The kind of texture (from the above possible).
    uint32_t kind;

    /// If the texture is an image, the width (in pixels).
    int16_t width;

    /// If the texture is an image, the height (in pixels).
    int16_t height;

    /// If this texture is procedural, the code we should run.
    std::string code;

    /// If the texture is an image, store flat array of pixel values.
    std::vector<float> image;

    MSGPACK_DEFINE(id, kind, width, height, code, image);

    TOSTRINGABLE(Texture);
};

std::string ToString(const Texture& tex, const std::string& indent = "");

} // namespace fr

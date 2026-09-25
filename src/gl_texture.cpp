#include "gl_texture.hpp"

#include <GLFW/glfw3.h>

void GlTexture::reset() {
    if (id_) {
        glDeleteTextures(1, &id_);
        id_ = 0;
    }
}

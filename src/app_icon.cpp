#include "app_icon.hpp"

#include <GLFW/glfw3.h>

// 複数サイズの RGBA を埋め込む。ファイルとして持つと起動ディレクトリ次第で読めなく
// なるうえ、PNG デコーダも要る。アイコンは小さいので実行ファイルに焼き込む。
#include "app_icon_data.inc"

void set_window_icon(GLFWwindow* window) {
    constexpr int kCount = static_cast<int>(sizeof kIconSizes / sizeof kIconSizes[0]);

    GLFWimage images[kCount];
    for (int i = 0; i < kCount; ++i) {
        images[i].width  = kIconSizes[i];
        images[i].height = kIconSizes[i];
        // GLFWimage::pixels は非 const だが、glfwSetWindowIcon はこの場でコピーして
        // 書き換えないので、読み取り専用データを渡して問題ない。
        images[i].pixels = const_cast<unsigned char*>(kIconPixels[i]);
    }
    // ウィンドウマネージャが必要なサイズを選ぶ（X11 では全サイズが _NET_WM_ICON に入る）。
    glfwSetWindowIcon(window, kCount, images);
}

#include "plugin/qrcode.hpp"

#include <vector>

extern "C" {
#include "qrcode.h"
}

namespace agent::plugin {

namespace {

// 获取模块状态，越界返回 false（白色）
inline bool get_module(::QRCode* qr, int x, int y) {
  return (x >= 0 && x < qr->size && y >= 0 && y < qr->size) && qrcode_getModule(qr, x, y);
}

// 将 QR 码渲染为 Unicode 字符串
// 使用半块字符实现 2 行合并为 1 行，保持正确宽高比
std::string render(::QRCode* qr) {
  // 查表：[top_black * 2 + bottom_black] -> 字符
  // 00=全白(█), 01=上白下黑(▀), 10=上黑下白(▄), 11=全黑( )
  static const char* BLOCKS[] = {"█", "▀", "▄", " "};

  const int size = qr->size;
  const int width = size + 4;  // 含边框
  std::string result;
  result.reserve(width * (size / 2 + 4) * 4);  // 预分配

  // 顶部边框
  result.append(width * 3, ' ');
  for (int i = 0; i < width; ++i) result.replace(result.size() - width * 3 + i * 3, 3, "█");
  result += "\n";

  // 逐两行处理（包含上下边距）
  for (int y = -1; y < size + 1; y += 2) {
    result += "██";
    for (int x = 0; x < size; ++x) {
      int idx = get_module(qr, x, y) * 2 + get_module(qr, x, y + 1);
      result += BLOCKS[idx];
    }
    result += "██\n";
  }

  // 底部边框
  for (int i = 0; i < width; ++i) result += "█";
  result += "\n";

  return result;
}

}  // namespace

std::string QrCode::encode(const std::string& text) {
  // ECC_LOW 模式下各版本最大字符容量
  static constexpr int CAPACITY[] = {0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271};

  // 选择最小版本
  int version = 1;
  for (; version <= 10 && text.length() > CAPACITY[version]; ++version)
    ;
  if (version > 10) return "[Text too long for QR code]";

  // 生成 QR 码
  std::vector<uint8_t> buffer(qrcode_getBufferSize(version));
  ::QRCode qr;

  if (qrcode_initText(&qr, buffer.data(), version, ECC_LOW, text.c_str()) != 0) {
    return "[QR code generation failed]";
  }

  return render(&qr);
}

}  // namespace agent::plugin

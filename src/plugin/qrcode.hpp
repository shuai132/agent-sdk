#pragma once

#include <string>

namespace agent::plugin {

// QR 码生成器包装类
// 使用 thirdparty/QRCode 库，生成适合终端显示的 Unicode 字符串
class QrCode {
 public:
  // 从文本生成 Unicode 字符串形式的 QR 码
  // 使用 Unicode 半块字符渲染，适合终端显示
  static std::string encode(const std::string& text);
};

}  // namespace agent::plugin

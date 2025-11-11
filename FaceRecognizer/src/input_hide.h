#include <iostream>
#include <stdexcept>  // 引入异常处理
#include <string>

// --- 平台检测 ---
#ifdef _WIN32
#define PLATFORM_WINDOWS
#elif defined(__unix__) || defined(__unix) || \
    (defined(__APPLE__) && defined(__MACH__))
#define PLATFORM_UNIX_LIKE
#else
#warning "Unsupported platform for hidden input"
#endif

// --- 平台特定的头文件 ---
#ifdef PLATFORM_WINDOWS
#include <io.h>  // _isatty
#include <windows.h>
#define STDIN_FILENO 0
#elif defined(PLATFORM_UNIX_LIKE)
#include <sys/ioctl.h>  // ioctl
#include <termios.h>
#include <unistd.h>
#else
// 不支持的平台
#endif

class HiddenInputReader {
 private:
#ifdef PLATFORM_WINDOWS
  HANDLE hStdIn;
  DWORD originalMode;
#elif defined(PLATFORM_UNIX_LIKE)
  struct termios originalTermios;
#else
  // 不支持的平台
#endif

  bool isTerminal() const {
#ifdef PLATFORM_WINDOWS
    return _isatty(STDIN_FILENO) != 0;
#elif defined(PLATFORM_UNIX_LIKE)
    return isatty(STDIN_FILENO) != 0;
#else
    return true;  // 默认假设是终端，或者返回 false?
#endif
  }

 public:
  HiddenInputReader() {
    if (!isTerminal()) {
      throw std::runtime_error(
          "Standard input is not connected to a terminal. Cannot hide input.");
    }

#ifdef PLATFORM_WINDOWS
    hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdIn == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("Failed to get standard input handle.");
    }
    if (!GetConsoleMode(hStdIn, &originalMode)) {
      throw std::runtime_error("Failed to get console mode.");
    }
    DWORD newMode = originalMode & ~(ENABLE_ECHO_INPUT);
    if (!SetConsoleMode(hStdIn, newMode)) {
      throw std::runtime_error("Failed to set console mode to hide input.");
    }
#elif defined(PLATFORM_UNIX_LIKE)
    if (tcgetattr(STDIN_FILENO, &originalTermios) != 0) {
      throw std::runtime_error("Failed to get terminal attributes.");
    }
    struct termios newTermios = originalTermios;
    newTermios.c_lflag &= ～ECHO;  // 关闭回显
    newTermios.c_lflag |= ECHONL;  // 保留换行符回显 (可选，看需求)
    if (tcsetattr(STDIN_FILENO, TCSANOW, &newTermios) != 0) {
      throw std::runtime_error(
          "Failed to set terminal attributes to hide input.");
    }
#else
    throw std::runtime_error(
        "Hidden input is not implemented for this platform.");
#endif
  }

  ~HiddenInputReader() { restoreTerminal(); }

  void restoreTerminal() {
#ifdef PLATFORM_WINDOWS
    if (hStdIn != INVALID_HANDLE_VALUE) {
      SetConsoleMode(hStdIn, originalMode);  // 恢复原始模式
    }
#elif defined(PLATFORM_UNIX_LIKE)
    tcsetattr(STDIN_FILENO, TCSANOW, &originalTermios);  // 恢复原始设置
#endif
  }

  std::string readHiddenLine(const std::string& prompt = "") {
    std::cout << prompt;
    std::string line;
    char ch;

    while (std::cin.get(ch)) {
      if (ch == '\n' || ch == '\r') {
        break;                               // 输入结束
      } else if (ch == '\b' || ch == 127) {  // 退格
        if (!line.empty()) {
          line.pop_back();
          // 简单的退格回显处理，可能不完美
          if (isTerminal()) {  // 只有在终端才尝试控制光标
            std::cout << "\b \b" << std::flush;
          }
        }
      } else {
        line.push_back(ch);
        if (isTerminal()) {                // 只有在终端才尝试显示符号
          std::cout << '*' << std::flush;  // 显示掩码
        }
      }
    }
    if (isTerminal()) {
      std::cout << std::endl;  // 换行
    }
    return line;
  }
};

// int main() {
//   try {
//     std::string password =
//         HiddenInputReader{}.readHiddenLine("请输入密码 (输入将不可见): ");
//     std::cout << "密码输入完成 (长度: " << password.length() << ")."
//               << std::endl;
//     // 实际应用中不要打印密码内容
//   } catch (const std::exception& e) {
//     std::cerr << "错误: " << e.what() << std::endl;
//     return 1;
//   }

//   return 0;
// }

#include <windows.h>
#include <string>
#include <vector>

/**
 * 注册表操作类，提供读取和写入注册表的功能
 */
class RegistryHelper {
public:
    /**
     * 写入字符串值到注册表
     * @param registryPath 注册表路径，格式如 "HKEY_CURRENT_USER\\Software\\MyApp\\ValueName"
     * @param value 要写入的字符串值
     * @return 操作是否成功
     */
    static bool WriteStringToRegistry(const std::string& registryPath, const std::string& value) {
        HKEY hKey;
        std::string keyPath, valueName;
        
        // 分离键路径和值名称
        if (!ParseRegistryPath(registryPath, hKey, keyPath, valueName)) {
            return false;
        }
        
        // 打开或创建注册表项
        HKEY hOpenKey;
        DWORD disposition;
        LONG result = RegCreateKeyExA(hKey, keyPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE,
                                      KEY_WRITE, NULL, &hOpenKey, &disposition);
        
        if (result != ERROR_SUCCESS) {
            return false;
        }
        
        // 写入字符串值
        result = RegSetValueExA(hOpenKey, valueName.c_str(), 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>(value.length() + 1));
        
        RegCloseKey(hOpenKey);
        
        return result == ERROR_SUCCESS;
    }
    
    /**
     * 读取注册表中的字符串值
     * @param registryPath 注册表路径，格式如 "HKEY_CURRENT_USER\\Software\\MyApp\\ValueName"
     * @param defaultValue 默认值，如果无法读取则返回此值
     * @return 读取到的字符串值
     */
    static std::string ReadStringFromRegistry(const std::string& registryPath, const std::string& defaultValue = "") {
        HKEY hKey;
        std::string keyPath, valueName;
        
        // 分离键路径和值名称
        if (!ParseRegistryPath(registryPath, hKey, keyPath, valueName)) {
            return defaultValue;
        }
        
        // 打开注册表项
        HKEY hOpenKey;
        LONG result = RegOpenKeyExA(hKey, keyPath.c_str(), 0, KEY_READ, &hOpenKey);
        
        if (result != ERROR_SUCCESS) {
            return defaultValue;
        }
        
        // 获取值的大小
        DWORD type, dataSize = 0;
        result = RegQueryValueExA(hOpenKey, valueName.c_str(), NULL, &type, NULL, &dataSize);
        
        if (result != ERROR_SUCCESS || type != REG_SZ) {
            RegCloseKey(hOpenKey);
            return defaultValue;
        }
        
        // 读取值
        std::vector<char> buffer(dataSize);
        result = RegQueryValueExA(hOpenKey, valueName.c_str(), NULL, &type,
                                  reinterpret_cast<LPBYTE>(buffer.data()), &dataSize);
        
        RegCloseKey(hOpenKey);
        
        if (result != ERROR_SUCCESS) {
            return defaultValue;
        }
        
        return std::string(buffer.data());
    }
    
    /**
     * 写入DWORD值到注册表
     * @param registryPath 注册表路径，格式如 "HKEY_CURRENT_USER\\Software\\MyApp\\ValueName"
     * @param value 要写入的DWORD值
     * @return 操作是否成功
     */
    static bool WriteDwordToRegistry(const std::string& registryPath, DWORD value) {
        HKEY hKey;
        std::string keyPath, valueName;
        
        // 分离键路径和值名称
        if (!ParseRegistryPath(registryPath, hKey, keyPath, valueName)) {
            return false;
        }
        
        // 打开或创建注册表项
        HKEY hOpenKey;
        DWORD disposition;
        LONG result = RegCreateKeyExA(hKey, keyPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE,
                                      KEY_WRITE, NULL, &hOpenKey, &disposition);
        
        if (result != ERROR_SUCCESS) {
            return false;
        }
        
        // 写入DWORD值
        result = RegSetValueExA(hOpenKey, valueName.c_str(), 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&value),
                                sizeof(DWORD));
        
        RegCloseKey(hOpenKey);
        
        return result == ERROR_SUCCESS;
    }
    
    /**
     * 读取注册表中的DWORD值
     * @param registryPath 注册表路径，格式如 "HKEY_CURRENT_USER\\Software\\MyApp\\ValueName"
     * @param defaultValue 默认值，如果无法读取则返回此值
     * @return 读取到的DWORD值
     */
    static DWORD ReadDwordFromRegistry(const std::string& registryPath, DWORD defaultValue = 0) {
        HKEY hKey;
        std::string keyPath, valueName;
        
        // 分离键路径和值名称
        if (!ParseRegistryPath(registryPath, hKey, keyPath, valueName)) {
            return defaultValue;
        }
        
        // 打开注册表项
        HKEY hOpenKey;
        LONG result = RegOpenKeyExA(hKey, keyPath.c_str(), 0, KEY_READ, &hOpenKey);
        
        if (result != ERROR_SUCCESS) {
            return defaultValue;
        }
        
        // 读取DWORD值
        DWORD type, dataSize = sizeof(DWORD);
        DWORD value;
        result = RegQueryValueExA(hOpenKey, valueName.c_str(), NULL, &type,
                                  reinterpret_cast<LPBYTE>(&value), &dataSize);
        
        RegCloseKey(hOpenKey);
        
        if (result != ERROR_SUCCESS || type != REG_DWORD) {
            return defaultValue;
        }
        
        return value;
    }
    
private:
    /**
     * 解析注册表路径，分离出根键、子键和值名
     * @param registryPath 完整的注册表路径
     * @param hKey 输出：根键句柄
     * @param keyPath 输出：子键路径
     * @param valueName 输出：值名称
     * @return 是否解析成功
     */
    static bool ParseRegistryPath(const std::string& registryPath, HKEY& hKey, std::string& keyPath, std::string& valueName) {
        size_t pos = registryPath.find_last_of('\\');
        if (pos == std::string::npos) {
            return false;
        }
        
        std::string fullKeyPath = registryPath.substr(0, pos);
        valueName = registryPath.substr(pos + 1);
        
        // 解析根键
        pos = fullKeyPath.find('\\');
        if (pos == std::string::npos) {
            return false;
        }
        
        std::string rootKeyName = fullKeyPath.substr(0, pos);
        keyPath = fullKeyPath.substr(pos + 1);
        
        // 根据根键名称确定HKEY句柄
        if (rootKeyName == "HKEY_LOCAL_MACHINE" || rootKeyName == "HKLM") {
            hKey = HKEY_LOCAL_MACHINE;
        } else if (rootKeyName == "HKEY_CURRENT_USER" || rootKeyName == "HKCU") {
            hKey = HKEY_CURRENT_USER;
        } else if (rootKeyName == "HKEY_CLASSES_ROOT" || rootKeyName == "HKCR") {
            hKey = HKEY_CLASSES_ROOT;
        } else if (rootKeyName == "HKEY_USERS") {
            hKey = HKEY_USERS;
        } else if (rootKeyName == "HKEY_CURRENT_CONFIG") {
            hKey = HKEY_CURRENT_CONFIG;
        } else {
            return false;
        }
        
        return true;
    }
};
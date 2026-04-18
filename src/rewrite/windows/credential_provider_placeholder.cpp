#include <windows.h>

extern "C" __declspec(dllexport) HRESULT RewriteCredentialProviderPlaceholder() {
    return S_OK;
}

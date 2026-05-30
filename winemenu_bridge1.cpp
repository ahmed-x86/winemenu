#include <windows.h>
#include <shlobj.h>
#include <iostream>
#include <string>

// دالة بسيطة لتنظيف النصوص وجعلها متوافقة مع JSON
std::string escape_json(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else if (c == '\b') result += "\\b";
        else if (c == '\f') result += "\\f";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else result += c;
    }
    return result;
}

// دالة تفاعلية لاستخراج القوائم (بما فيها القوائم الفرعية Submenus) وطباعتها كـ JSON
void parse_menu(HMENU hMenu, IContextMenu* pCtx, IContextMenu2* pCtx2, IContextMenu3* pCtx3, int indent) {
    int count = GetMenuItemCount(hMenu);
    std::cout << "[\n";
    bool first = true;
    for (int i = 0; i < count; i++) {
        MENUITEMINFOA mii = { sizeof(mii) };
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE | MIIM_STATE;
        char buf[512] = {0};
        mii.dwTypeData = buf;
        mii.cch = sizeof(buf) - 1;

        if (!GetMenuItemInfoA(hMenu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) continue; // تجاهل الفواصل (الخطوط)
        
        std::string text = escape_json(buf);
        if (text.empty()) continue;

        if (!first) std::cout << ",\n";
        first = false;

        std::cout << std::string(indent + 2, ' ') << "{\n";
        std::cout << std::string(indent + 4, ' ') << "\"id\": " << (mii.wID - 1) << ",\n";
        
        // إزالة علامة '&' التي يستخدمها ويندوز للاختصارات في الكيبورد لتنظيف الواجهة
        std::string clean_text = "";
        for(char c : text) if(c != '&') clean_text += c;
        
        std::cout << std::string(indent + 4, ' ') << "\"name\": \"" << clean_text << "\"";

        // إذا كان هناك قائمة فرعية (مثل Extract to...)
        if (mii.hSubMenu) {
            std::cout << ",\n" << std::string(indent + 4, ' ') << "\"submenu\": ";
            
            // إجبار برامج (مثل WinRAR) على توليد عناصر القائمة الفرعية ديناميكياً
            if (pCtx3) {
                LRESULT lres;
                pCtx3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)mii.hSubMenu, i, &lres);
            } else if (pCtx2) {
                pCtx2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)mii.hSubMenu, i);
            }
            
            parse_menu(mii.hSubMenu, pCtx, pCtx2, pCtx3, indent + 4);
        }
        std::cout << "\n" << std::string(indent + 2, ' ') << "}";
    }
    std::cout << "\n" << std::string(indent, ' ') << "]";
}

int main(int argc, char* argv[]) {
    // الأداة تقبل وضعين: list (للاستخراج) أو invoke (للتنفيذ)
    if (argc < 3) {
        std::cerr << "Usage: winemenu_bridge.exe <list|invoke> <Windows_Path> [idCmd]\n";
        return 1;
    }

    std::string mode = argv[1];
    std::string path = argv[2];
    int action_id = -1;

    if (mode == "invoke" && argc >= 4) {
        action_id = std::stoi(argv[3]);
    }

    // تهيئة بيئة COM
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    IShellFolder* pDesktop = NULL;
    if (FAILED(SHGetDesktopFolder(&pDesktop))) return 1;

    // تحويل المسار إلى Wide String
    int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    std::wstring wPath(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wPath[0], len);

    // الحصول على معرف الملف (PIDL)
    PIDLIST_ABSOLUTE pidl = NULL;
    if (FAILED(pDesktop->ParseDisplayName(NULL, NULL, &wPath[0], NULL, &pidl, NULL))) {
        std::cerr << "Error: File not found or invalid path.\n";
        return 1;
    }

    IShellFolder* pParent = NULL;
    PCUITEMID_CHILD pidlChild = NULL;
    if (FAILED(SHBindToParent(pidl, IID_IShellFolder, (void**)&pParent, &pidlChild))) return 1;

    // استخراج كائن IContextMenu الخاص بالملف
    IContextMenu* pCtx = NULL;
    if (FAILED(pParent->GetUIObjectOf(NULL, 1, &pidlChild, IID_IContextMenu, NULL, (void**)&pCtx))) return 1;

    HMENU hMenu = CreatePopupMenu();
    // طلب إنشاء القائمة ووضع العناصر فيها (من ID 1 إلى 0x7FFF)
    if (FAILED(pCtx->QueryContextMenu(hMenu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXPLORE))) return 1;

    if (mode == "list") {
        IContextMenu2* pCtx2 = NULL;
        IContextMenu3* pCtx3 = NULL;
        pCtx->QueryInterface(IID_IContextMenu2, (void**)&pCtx2);
        pCtx->QueryInterface(IID_IContextMenu3, (void**)&pCtx3);

        parse_menu(hMenu, pCtx, pCtx2, pCtx3, 0);

        if (pCtx3) pCtx3->Release();
        if (pCtx2) pCtx2->Release();
    } 
    else if (mode == "invoke" && action_id >= 0) {
        CMINVOKECOMMANDINFOEX cmi = {0};
        cmi.cbSize = sizeof(cmi);
        cmi.fMask = CMIC_MASK_ASYNCOK | CMIC_MASK_FLAG_NO_UI;
        cmi.lpVerb = (LPCSTR)MAKEINTRESOURCE(action_id); // إرسال الـ ID للأمر المستهدف
        cmi.nShow = SW_SHOWNORMAL;
        
        // تنفيذ الأمر
        HRESULT hr = pCtx->InvokeCommand((CMINVOKECOMMANDINFO*)&cmi);
        if (SUCCEEDED(hr)) {
            std::cout << "Success: Command invoked.\n";
        } else {
            std::cerr << "Error: Command failed (HRESULT: " << hr << ").\n";
        }
    }

    // تنظيف الذاكرة
    DestroyMenu(hMenu);
    pCtx->Release();
    pParent->Release();
    CoTaskMemFree(pidl);
    pDesktop->Release();
    CoUninitialize();

    return 0;
}
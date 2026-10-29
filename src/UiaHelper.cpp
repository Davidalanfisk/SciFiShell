// SciFiUia: performs UI Automation clicks for the shell in a separate process.
// Notepad dismisses flyouts that are opened by an in-process UI Automation client, so the shell asks this helper.
//
// stdin:  "C <x> <y>"        click the actionable element at screen point (x, y)
//         "F <hwnd> <x> <y>" click the actionable element at (x, y) inside the flyout window <hwnd>
//         "X"                collapse the last menu this helper opened
// stdout: "R <ok> <opened> <left> <top> <right> <bottom>" for every C / F request
#define NOMINMAX
#include <windows.h>

#include <UIAutomation.h>

#include <cstdio>
#include <cstdlib>

static bool hasProp(IUIAutomationElement* e, PROPERTYID id) {
    VARIANT v;
    VariantInit(&v);
    bool r = SUCCEEDED(e->GetCurrentPropertyValue(id, &v)) && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
    VariantClear(&v);
    return r;
}

static bool actionable(IUIAutomationElement* e) {
    return hasProp(e, UIA_IsInvokePatternAvailablePropertyId) || hasProp(e, UIA_IsExpandCollapsePatternAvailablePropertyId) ||
           hasProp(e, UIA_IsTogglePatternAvailablePropertyId) || hasProp(e, UIA_IsSelectionItemPatternAvailablePropertyId);
}

// Performs the element's natural action. `opened` is set when it expanded a menu or drop-down.
static bool activate(IUIAutomationElement* e, bool& opened) {
    opened = false;
    IUIAutomationExpandCollapsePattern* ec = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, __uuidof(IUIAutomationExpandCollapsePattern),
                                         reinterpret_cast<void**>(&ec))) && ec) {
        ExpandCollapseState st = ExpandCollapseState_LeafNode;
        ec->get_CurrentExpandCollapseState(&st);
        HRESULT hr = E_FAIL;
        if (st == ExpandCollapseState_Collapsed) {
            hr = ec->Expand();
            opened = SUCCEEDED(hr);
        } else if (st != ExpandCollapseState_LeafNode) {
            hr = ec->Collapse();
        }
        ec->Release();
        if (SUCCEEDED(hr)) return true;
    }
    IUIAutomationInvokePattern* inv = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void**>(&inv))) &&
        inv) {
        HRESULT hr = inv->Invoke();
        inv->Release();
        if (SUCCEEDED(hr)) return true;
    }
    IUIAutomationSelectionItemPattern* sel = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionItemPatternId, __uuidof(IUIAutomationSelectionItemPattern),
                                         reinterpret_cast<void**>(&sel))) && sel) {
        HRESULT hr = sel->Select();
        sel->Release();
        if (SUCCEEDED(hr)) return true;
    }
    IUIAutomationTogglePattern* tg = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TogglePatternId, __uuidof(IUIAutomationTogglePattern), reinterpret_cast<void**>(&tg))) &&
        tg) {
        HRESULT hr = tg->Toggle();
        tg->Release();
        if (SUCCEEDED(hr)) return true;
    }
    return false;
}

// The smallest actionable element under a screen point, searching below `root` (a flyout) or, when null, hit-testing.
static IUIAutomationElement* actionableAt(IUIAutomation* a, IUIAutomationElement* root, POINT pt) {
    if (!root) {
        IUIAutomationElement* e = nullptr;
        if (FAILED(a->ElementFromPoint(pt, &e)) || !e) return nullptr;
        IUIAutomationTreeWalker* walker = nullptr;
        a->get_ControlViewWalker(&walker);
        for (int i = 0; i < 8 && e; ++i) {
            if (actionable(e)) {
                if (walker) walker->Release();
                return e;
            }
            IUIAutomationElement* p = nullptr;
            if (walker) walker->GetParentElement(e, &p);
            e->Release();
            e = p;
        }
        if (walker) walker->Release();
        return nullptr;
    }
    static const PROPERTYID ids[4] = {UIA_IsInvokePatternAvailablePropertyId, UIA_IsExpandCollapsePatternAvailablePropertyId,
                                      UIA_IsTogglePatternAvailablePropertyId, UIA_IsSelectionItemPatternAvailablePropertyId};
    IUIAutomationCondition* cs[4] = {};
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    for (int i = 0; i < 4; ++i) a->CreatePropertyCondition(ids[i], v, &cs[i]);
    IUIAutomationCondition* any = nullptr;
    a->CreateOrConditionFromNativeArray(cs, 4, &any);
    IUIAutomationElement* best = nullptr;
    long bestArea = 0x7fffffff;
    if (any) {
        IUIAutomationElementArray* arr = nullptr;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, any, &arr)) && arr) {
            int n = 0;
            arr->get_Length(&n);
            for (int i = 0; i < n; ++i) {
                IUIAutomationElement* e = nullptr;
                if (FAILED(arr->GetElement(i, &e)) || !e) continue;
                RECT r{};
                e->get_CurrentBoundingRectangle(&r);
                const long area = (r.right - r.left) * (r.bottom - r.top);
                if (PtInRect(&r, pt) && area > 0 && area < bestArea) {
                    if (best) best->Release();
                    best = e;
                    bestArea = area;
                } else {
                    e->Release();
                }
            }
            arr->Release();
        }
        any->Release();
    }
    for (auto* c : cs)
        if (c) c->Release();
    return best;
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
    if (!uia) return 1;
    IUIAutomationElement* lastExpanded = nullptr;
    char line[256];
    while (std::fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'X') {
            if (lastExpanded) {
                IUIAutomationExpandCollapsePattern* ec = nullptr;
                if (SUCCEEDED(lastExpanded->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
                                                                __uuidof(IUIAutomationExpandCollapsePattern),
                                                                reinterpret_cast<void**>(&ec))) && ec) {
                    ec->Collapse();
                    ec->Release();
                }
            }
            continue;
        }
        POINT pt{};
        unsigned long long hw = 0;
        IUIAutomationElement* root = nullptr;
        if (line[0] == 'C') {
            if (std::sscanf(line + 1, "%ld %ld", &pt.x, &pt.y) != 2) continue;
        } else if (line[0] == 'F') {
            if (std::sscanf(line + 1, "%llx %ld %ld", &hw, &pt.x, &pt.y) != 3) continue;
            uia->ElementFromHandle(reinterpret_cast<HWND>(static_cast<uintptr_t>(hw)), &root);
        } else {
            continue;
        }
        int ok = 0, opened = 0;
        RECT er{};
        if (line[0] == 'C' || root) {
            if (IUIAutomationElement* e = actionableAt(uia, root, pt)) {
                e->get_CurrentBoundingRectangle(&er);
                bool op = false;
                ok = activate(e, op) ? 1 : 0;
                opened = op ? 1 : 0;
                if (ok && op) {
                    if (lastExpanded) lastExpanded->Release();
                    lastExpanded = e;
                } else {
                    e->Release();
                }
            }
        }
        if (root) root->Release();
        std::printf("R %d %d %ld %ld %ld %ld\n", ok, opened, er.left, er.top, er.right, er.bottom);
        std::fflush(stdout);
    }
    if (lastExpanded) lastExpanded->Release();
    uia->Release();
    CoUninitialize();
    return 0;
}

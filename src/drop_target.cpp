#include "drop_target.h"
#include <shellapi.h>

DropTarget::DropTarget(DropCallback callback)
    : m_refCount(1), m_callback(callback) {
}

DropTarget::~DropTarget() {
}

HRESULT STDMETHODCALLTYPE DropTarget::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject)
        return E_INVALIDARG;

    *ppvObject = nullptr;

    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *ppvObject = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DropTarget::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE DropTarget::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragEnter(IDataObject* pDataObj, DWORD /*grfKeyState*/,
                                                 POINTL /*pt*/, DWORD* pdwEffect) {
    if (!pDataObj || !pdwEffect)
        return E_INVALIDARG;

    // Check if data format is HDROP (files/folders)
    FORMATETC fmtetc = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (pDataObj->QueryGetData(&fmtetc) == S_OK) {
        *pdwEffect = DROPEFFECT_COPY;
    } else {
        *pdwEffect = DROPEFFECT_NONE;
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragOver(DWORD /*grfKeyState*/, POINTL /*pt*/,
                                                DWORD* pdwEffect) {
    if (!pdwEffect)
        return E_INVALIDARG;

    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragLeave() {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::Drop(IDataObject* pDataObj, DWORD /*grfKeyState*/,
                                            POINTL /*pt*/, DWORD* pdwEffect) {
    if (!pDataObj || !pdwEffect)
        return E_INVALIDARG;

    *pdwEffect = DROPEFFECT_NONE;

    std::wstring path;
    if (ExtractDroppedPath(pDataObj, path)) {
        if (m_callback) {
            m_callback(path);
        }
        *pdwEffect = DROPEFFECT_COPY;
    }

    return S_OK;
}

bool DropTarget::ExtractDroppedPath(IDataObject* pDataObj, std::wstring& outPath) {
    FORMATETC fmtetc = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stgmed = {};

    if (pDataObj->GetData(&fmtetc, &stgmed) != S_OK)
        return false;

    bool success = false;
    HDROP hDrop = static_cast<HDROP>(GlobalLock(stgmed.hGlobal));
    if (hDrop) {
        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        if (fileCount > 0) {
            wchar_t buffer[MAX_PATH];
            if (DragQueryFileW(hDrop, 0, buffer, MAX_PATH) > 0) {
                outPath = buffer;
                success = true;
            }
        }
        GlobalUnlock(stgmed.hGlobal);
    }

    ReleaseStgMedium(&stgmed);
    return success;
}

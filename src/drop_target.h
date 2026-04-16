#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ole2.h>
#include <functional>
#include <string>

class DropTarget : public IDropTarget {
public:
    using DropCallback = std::function<void(const std::wstring& path)>;

    explicit DropTarget(DropCallback callback);
    ~DropTarget();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState,
                                        POINTL pt, DWORD* pdwEffect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt,
                                       DWORD* pdwEffect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState,
                                   POINTL pt, DWORD* pdwEffect) override;

private:
    bool ExtractDroppedPath(IDataObject* pDataObj, std::wstring& outPath);

    ULONG m_refCount;
    DropCallback m_callback;
};

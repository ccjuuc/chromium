#pragma once
#include <ExDisp.h>
#include <ExDispid.h>
#include <mshtmhst.h>

namespace DuiLib
{
	class CWebBrowserUI;
	class CWebBrowserEventHandler
	{
	public:
		CWebBrowserEventHandler() {}
		~CWebBrowserEventHandler() {}

		virtual void BeforeNavigate2(CWebBrowserUI* pWeb, IDispatch *pDisp,VARIANT *&url,VARIANT *&Flags,VARIANT *&TargetFrameName,VARIANT *&PostData,VARIANT *&Headers,VARIANT_BOOL *&Cancel ) {}
		virtual void NavigateError(CWebBrowserUI* pWeb, IDispatch *pDisp,VARIANT * &url,VARIANT *&TargetFrameName,VARIANT *&StatusCode,VARIANT_BOOL *&Cancel) {}
		virtual void NavigateComplete2(CWebBrowserUI* pWeb, IDispatch *pDisp,VARIANT *&url){}
		virtual void ProgressChange(CWebBrowserUI* pWeb, LONG nProgress, LONG nProgressMax){}
		virtual void NewWindow3(CWebBrowserUI* pWeb, IDispatch **pDisp, VARIANT_BOOL *&Cancel, DWORD dwFlags, BSTR bstrUrlContext, BSTR bstrUrl){}
		virtual void CommandStateChange(CWebBrowserUI* pWeb, long Command,VARIANT_BOOL Enable){};
		virtual void TitleChange(CWebBrowserUI* pWeb, BSTR bstrTitle){};
		virtual void DocumentComplete(CWebBrowserUI* pWeb, IDispatch *pDisp,VARIANT *&url){}


		virtual void WebMessageReceived(CWebBrowserUI* pWeb, LPCWSTR lpszMessage) {}


		virtual HRESULT STDMETHODCALLTYPE ShowContextMenu(CWebBrowserUI* pWeb,
			 DWORD dwID,
			 POINT __RPC_FAR *ppt,
			 IUnknown __RPC_FAR *pcmdtReserved,
			 IDispatch __RPC_FAR *pdispReserved)
		{


			return S_OK;

		}

		virtual HRESULT STDMETHODCALLTYPE GetHostInfo(CWebBrowserUI* pWeb,
			 DOCHOSTUIINFO __RPC_FAR *pInfo)
		{
			return E_NOTIMPL;
		}

		virtual HRESULT STDMETHODCALLTYPE ShowUI(CWebBrowserUI* pWeb,
			 DWORD dwID,
			 IOleInPlaceActiveObject __RPC_FAR *pActiveObject,
			 IOleCommandTarget __RPC_FAR *pCommandTarget,
			 IOleInPlaceFrame __RPC_FAR *pFrame,
			 IOleInPlaceUIWindow __RPC_FAR *pDoc)
		{
			return S_FALSE;
		}

		virtual HRESULT STDMETHODCALLTYPE HideUI( CWebBrowserUI* pWeb)
		{
			return S_OK;
		}

		virtual HRESULT STDMETHODCALLTYPE UpdateUI( CWebBrowserUI* pWeb)
		{
			return S_OK;
		}

		virtual HRESULT STDMETHODCALLTYPE EnableModeless(CWebBrowserUI* pWeb,
			 BOOL fEnable)
		{
			return S_OK;
		}

		virtual HRESULT STDMETHODCALLTYPE OnDocWindowActivate(CWebBrowserUI* pWeb,
			 BOOL fActivate)
		{
			return S_OK;
		}

		virtual HRESULT STDMETHODCALLTYPE OnFrameWindowActivate(CWebBrowserUI* pWeb,
			 BOOL fActivate)
		{
			return S_OK;
		}

		virtual HRESULT STDMETHODCALLTYPE ResizeBorder(CWebBrowserUI* pWeb,
			 LPCRECT prcBorder,
			 IOleInPlaceUIWindow __RPC_FAR *pUIWindow,
			 BOOL fRameWindow)
		{
			return S_OK;
		}

		virtual HRESULT STDMETHODCALLTYPE TranslateAccelerator(CWebBrowserUI* pWeb,
			 LPMSG lpMsg,
			 const GUID __RPC_FAR *pguidCmdGroup,
			 DWORD nCmdID)
		{
			return S_FALSE;
		}

		virtual HRESULT STDMETHODCALLTYPE GetOptionKeyPath(CWebBrowserUI* pWeb,
			 LPOLESTR __RPC_FAR *pchKey,
			 DWORD dw)
		{
			return S_OK;
		}

		virtual HRESULT STDMETHODCALLTYPE GetDropTarget(CWebBrowserUI* pWeb,
			 IDropTarget __RPC_FAR *pDropTarget,
			 IDropTarget __RPC_FAR *__RPC_FAR *ppDropTarget)
		{
			return E_NOTIMPL;
		}

		virtual HRESULT STDMETHODCALLTYPE GetExternal(CWebBrowserUI* pWeb,
			 IDispatch __RPC_FAR *__RPC_FAR *ppDispatch)
		{
			return E_NOTIMPL;
		}

		virtual HRESULT STDMETHODCALLTYPE TranslateUrl(CWebBrowserUI* pWeb,
			 DWORD dwTranslate,
			 OLECHAR __RPC_FAR *pchURLIn,
			 OLECHAR __RPC_FAR *__RPC_FAR *ppchURLOut)
		{
			return S_OK;
		}

		virtual HRESULT STDMETHODCALLTYPE FilterDataObject(CWebBrowserUI* pWeb,
			 IDataObject __RPC_FAR *pDO,
			 IDataObject __RPC_FAR *__RPC_FAR *ppDORet)
		{
			return S_OK;
		}










		virtual HRESULT STDMETHODCALLTYPE Download( CWebBrowserUI* pWeb,
			 IMoniker *pmk,
			 IBindCtx *pbc,
			 DWORD dwBindVerb,
			 LONG grfBINDF,
			 BINDINFO *pBindInfo,
			 LPCOLESTR pszHeaders,
			 LPCOLESTR pszRedir,
			 UINT uiCP)
		{
			return S_OK;
		}
	};
}

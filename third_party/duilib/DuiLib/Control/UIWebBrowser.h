#ifndef __UIWEBBROWSER_H__
#define __UIWEBBROWSER_H__

#pragma once

#include <MsHTML.h>
#include "Utils/WebBrowserEventHandler.h"
#include <ExDisp.h>


#if !defined(DUILIB_USE_WEBVIEW2)
#  if defined(_MSC_VER) && (_MSC_VER >= 1900)
#    if defined(__has_include)
#      if __has_include("../WebView2.h") && __has_include("../WebView2EnvironmentOptions.h")
#        define DUILIB_USE_WEBVIEW2 1
#      endif
#    endif
#  endif
#endif

#if defined(DUILIB_USE_WEBVIEW2)

#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include "Utils/WebView2Helper.h"
#endif

namespace DuiLib
{
	class UILIB_API CWebBrowserUI
		: public CActiveXUI
		, public IDocHostUIHandler
		, public IServiceProvider
		, public IOleCommandTarget
		, public IDispatch
		, public ITranslateAccelerator
		, public IInternetSecurityManager
	{
		DECLARE_DUICONTROL(CWebBrowserUI)
	public:

		CWebBrowserUI();
		virtual ~CWebBrowserUI();

		void SetHomePage(LPCTSTR lpszUrl);
		LPCTSTR GetHomePage();

		void SetAutoNavigation(bool bAuto = TRUE);
		bool IsAutoNavigation();

		void SetWebBrowserEventHandler(CWebBrowserEventHandler* pEventHandler);
		void Navigate2(LPCTSTR lpszUrl);
		void Refresh();
		void Refresh2(int Level);
		void GoBack();
		void GoForward();
		void NavigateHomePage();
		void NavigateUrl(LPCTSTR lpszUrl);
		virtual bool DoCreateControl();
		IWebBrowser2* GetWebBrowser2(void);
		IDispatch*		   GetHtmlWindow();
		static DISPID FindId(IDispatch *pObj, LPOLESTR pName);
		static HRESULT InvokeMethod(IDispatch *pObj, LPOLESTR pMehtod, VARIANT *pVarResult, VARIANT *ps, int cArgs);
		static HRESULT GetProperty(IDispatch *pObj, LPOLESTR pName, VARIANT *pValue);
		static HRESULT SetProperty(IDispatch *pObj, LPOLESTR pName, VARIANT *pValue);


#if defined(DUILIB_USE_WEBVIEW2)

		bool IsWebView2Mode() const;

		void SetWebView2Enabled(bool bEnable);

		void Stop();

		void PostWebMessage(LPCTSTR lpszMessage);

		bool IsWebView2Supported();

		ICoreWebView2* GetWebView2() const;

		enum { WEBVIEW2_MSG_FAILED = 1, WEBVIEW2_MSG_CREATE_CONTROLLER = 2, WEBVIEW2_MSG_BIND = 3 };
		static UINT GetWebView2NotifyMessage();

		virtual void SetPos(RECT rc, bool bNeedInvalidate = true);
		virtual void Move(SIZE szOffset, bool bNeedInvalidate = true);
		virtual void SetVisible(bool bVisible = true);
		virtual void SetInternVisible(bool bVisible = true);
		virtual LRESULT MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam, bool& bHandled);


		bool InitWebView2();
		void ReleaseWebView2();
		void UpdateWebView2Bounds();
		void OnWebView2Notify(WPARAM wParam, LPARAM lParam);
		void OnWebView2Bind();

		void OnWebView2NavigationStarting(ICoreWebView2* pWebView, ICoreWebView2NavigationStartingEventArgs* pArgs);
		void OnWebView2NavigationCompleted(ICoreWebView2* pWebView, ICoreWebView2NavigationCompletedEventArgs* pArgs);
		void OnWebView2DocumentTitleChanged(ICoreWebView2* pWebView);
		void OnWebView2NewWindowRequested(ICoreWebView2* pWebView, ICoreWebView2NewWindowRequestedEventArgs* pArgs);
		void OnWebView2HistoryChanged(ICoreWebView2* pWebView);
		void OnWebView2WebMessageReceived(ICoreWebView2* pWebView, ICoreWebView2WebMessageReceivedEventArgs* pArgs);
#endif

	protected:
		IWebBrowser2*			m_pWebBrowser2;
		IHTMLWindow2*		_pHtmlWnd2;
		LONG m_dwRef;
		DWORD m_dwCookie;
		virtual void ReleaseControl();
		HRESULT RegisterEventHandler(BOOL inAdvise);
		virtual void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);
		CDuiString m_sHomePage;
		bool m_bAutoNavi;
		CWebBrowserEventHandler* m_pWebBrowserEventHandler;


		void BeforeNavigate2( IDispatch *pDisp,VARIANT *&url,VARIANT *&Flags,VARIANT *&TargetFrameName,VARIANT *&PostData,VARIANT *&Headers,VARIANT_BOOL *&Cancel );
		void NavigateError(IDispatch *pDisp,VARIANT * &url,VARIANT *&TargetFrameName,VARIANT *&StatusCode,VARIANT_BOOL *&Cancel);
		void NavigateComplete2(IDispatch *pDisp,VARIANT *&url);
		void ProgressChange(LONG nProgress, LONG nProgressMax);
		void NewWindow3(IDispatch **pDisp, VARIANT_BOOL *&Cancel, DWORD dwFlags, BSTR bstrUrlContext, BSTR bstrUrl);
		void CommandStateChange(long Command,VARIANT_BOOL Enable);
		void TitleChange(BSTR bstrTitle);
		void DocumentComplete(IDispatch *pDisp,VARIANT *&url);


		bool DoCreateActiveXBrowser();

	public:
		virtual LPCTSTR GetClass() const;
		virtual LPVOID GetInterface( LPCTSTR pstrName );


		STDMETHOD_(ULONG,AddRef)();
		STDMETHOD_(ULONG,Release)();
		STDMETHOD(QueryInterface)(REFIID riid, LPVOID *ppvObject);


		virtual HRESULT STDMETHODCALLTYPE GetTypeInfoCount( __RPC__out UINT *pctinfo );
		virtual HRESULT STDMETHODCALLTYPE GetTypeInfo( UINT iTInfo, LCID lcid, __RPC__deref_out_opt ITypeInfo **ppTInfo );
		virtual HRESULT STDMETHODCALLTYPE GetIDsOfNames( __RPC__in REFIID riid, __RPC__in_ecount_full(cNames ) LPOLESTR *rgszNames, UINT cNames, LCID lcid, __RPC__out_ecount_full(cNames) DISPID *rgDispId);
		virtual HRESULT STDMETHODCALLTYPE Invoke( DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr );


		STDMETHOD(ShowContextMenu)(DWORD dwID, POINT* pptPosition, IUnknown* pCommandTarget, IDispatch* pDispatchObjectHit);
		STDMETHOD(GetHostInfo)(DOCHOSTUIINFO* pInfo);
		STDMETHOD(ShowUI)(DWORD dwID, IOleInPlaceActiveObject* pActiveObject, IOleCommandTarget* pCommandTarget, IOleInPlaceFrame* pFrame, IOleInPlaceUIWindow* pDoc);
		STDMETHOD(HideUI)();
		STDMETHOD(UpdateUI)();
		STDMETHOD(EnableModeless)(BOOL fEnable);
		STDMETHOD(OnDocWindowActivate)(BOOL fActivate);
		STDMETHOD(OnFrameWindowActivate)(BOOL fActivate);
		STDMETHOD(ResizeBorder)(LPCRECT prcBorder, IOleInPlaceUIWindow* pUIWindow, BOOL fFrameWindow);
		STDMETHOD(TranslateAccelerator)(LPMSG lpMsg, const GUID* pguidCmdGroup, DWORD nCmdID);
		STDMETHOD(GetOptionKeyPath)(LPOLESTR* pchKey, DWORD dwReserved);
		STDMETHOD(GetDropTarget)(IDropTarget* pDropTarget, IDropTarget** ppDropTarget);
		STDMETHOD(GetExternal)(IDispatch** ppDispatch);
		STDMETHOD(TranslateUrl)(DWORD dwTranslate, OLECHAR* pchURLIn, OLECHAR** ppchURLOut);
		STDMETHOD(FilterDataObject)(IDataObject* pDO, IDataObject** ppDORet);


		STDMETHOD(QueryService)(REFGUID guidService, REFIID riid, void** ppvObject);


		virtual HRESULT STDMETHODCALLTYPE QueryStatus( __RPC__in_opt const GUID *pguidCmdGroup, ULONG cCmds, __RPC__inout_ecount_full(cCmds ) OLECMD prgCmds[ ], __RPC__inout_opt OLECMDTEXT *pCmdText);
		virtual HRESULT STDMETHODCALLTYPE Exec( __RPC__in_opt const GUID *pguidCmdGroup, DWORD nCmdID, DWORD nCmdexecopt, __RPC__in_opt VARIANT *pvaIn, __RPC__inout_opt VARIANT *pvaOut );


		STDMETHOD(Download)(
			 IMoniker *pmk,
			 IBindCtx *pbc,
			 DWORD dwBindVerb,
			 LONG grfBINDF,
			 BINDINFO *pBindInfo,
			 LPCOLESTR pszHeaders,
			 LPCOLESTR pszRedir,
			 UINT uiCP);

		virtual HRESULT STDMETHODCALLTYPE SetSecuritySite(
             __RPC__in_opt IInternetSecurityMgrSite *pSite){return S_OK;}

        virtual HRESULT STDMETHODCALLTYPE GetSecuritySite(
             __RPC__deref_out_opt IInternetSecurityMgrSite **ppSite){return S_OK;}

        virtual HRESULT STDMETHODCALLTYPE MapUrlToZone(
             __RPC__in LPCWSTR pwszUrl,
             __RPC__out DWORD *pdwZone,
			 DWORD dwFlags) {return S_OK;}

        virtual HRESULT STDMETHODCALLTYPE GetSecurityId(
             __RPC__in LPCWSTR pwszUrl,
             __RPC__out_ecount_full(*pcbSecurityId) BYTE *pbSecurityId,
             __RPC__inout DWORD *pcbSecurityId,
             DWORD_PTR dwReserved) {return S_OK;}

        virtual HRESULT STDMETHODCALLTYPE ProcessUrlAction(
             __RPC__in LPCWSTR pwszUrl,
             DWORD dwAction,
             __RPC__out_ecount_full(cbPolicy) BYTE *pPolicy,
             DWORD cbPolicy,
             __RPC__in_opt BYTE *pContext,
             DWORD cbContext,
             DWORD dwFlags,
			 DWORD dwReserved)
		{
			return S_OK;
		}

        virtual HRESULT STDMETHODCALLTYPE QueryCustomPolicy(
             __RPC__in LPCWSTR pwszUrl,
             __RPC__in REFGUID guidKey,
             __RPC__deref_out_ecount_full_opt(*pcbPolicy) BYTE **ppPolicy,
             __RPC__out DWORD *pcbPolicy,
             __RPC__in BYTE *pContext,
             DWORD cbContext,
             DWORD dwReserved) {return S_OK;}

        virtual HRESULT STDMETHODCALLTYPE SetZoneMapping(
             DWORD dwZone,
             __RPC__in LPCWSTR lpszPattern,
             DWORD dwFlags) {return S_OK;}

        virtual HRESULT STDMETHODCALLTYPE GetZoneMappings(
             DWORD dwZone,
             __RPC__deref_out_opt IEnumString **ppenumString,
             DWORD dwFlags) {return S_OK;}


		virtual LRESULT TranslateAccelerator( MSG *pMsg );

	private:
#if defined(DUILIB_USE_WEBVIEW2)
		bool m_bUseWebView2;
		ICoreWebView2Environment* m_pWebView2Env;
		ICoreWebView2Controller* m_pWebView2Controller;
		ICoreWebView2* m_pWebView2;
		UINT m_uWebView2NotifyMsg;
#endif
		CDuiString m_sPendingUrl;
		int m_nWebView2Mode;
	};
}
#endif

#include "UIlib.h"
#include "UIWebBrowser.h"
#include <atlconv.h>
#include <atlcomcli.h>
#include "../Utils/downloadmgr.h"
#include <mshtml.h>

namespace DuiLib
{


	IMPLEMENT_DUICONTROL(CWebBrowserUI)

		CWebBrowserUI::CWebBrowserUI()
		: m_pWebBrowser2(NULL)
		, _pHtmlWnd2(NULL)
		, m_pWebBrowserEventHandler(NULL)
		, m_bAutoNavi(false)
		, m_dwRef(0)
		, m_dwCookie(0)
#if defined(DUILIB_USE_WEBVIEW2)
		, m_bUseWebView2(false)
		, m_pWebView2Env(NULL)
		, m_pWebView2Controller(NULL)
		, m_pWebView2(NULL)
		, m_uWebView2NotifyMsg(0)
#endif
		, m_nWebView2Mode(-1)
	{
		m_clsid=CLSID_WebBrowser;
		m_sHomePage.Empty();
#if defined(DUILIB_USE_WEBVIEW2)



		static LONG s_nSeq = 0;
		TCHAR szMsg[128] = { 0 };
		_stprintf_s(szMsg, _T("DuiLib_WebView2_Notify_%p_%d"), (void*)this, (int)::InterlockedIncrement(&s_nSeq));
		m_uWebView2NotifyMsg = ::RegisterWindowMessage(szMsg);
		if (m_uWebView2NotifyMsg == 0)
			m_uWebView2NotifyMsg = ::RegisterWindowMessage(_T("DuiLib_WebView2_Notify"));
#endif
	}

	bool CWebBrowserUI::DoCreateControl()
	{
#if defined(DUILIB_USE_WEBVIEW2)

		if (m_nWebView2Mode != 0 && IsWebView2Supported()) {
			m_bUseWebView2 = true;
			m_bCreated = true;
			if (m_pManager != NULL) m_pManager->AddMessageFilter(this);
			return InitWebView2();
		}
		m_bUseWebView2 = false;
#endif
		return DoCreateActiveXBrowser();
	}


	bool CWebBrowserUI::DoCreateActiveXBrowser()
	{
		if (!CActiveXUI::DoCreateControl())
			return false;
		GetManager()->AddTranslateAccelerator(this);
		GetControl(IID_IWebBrowser2,(LPVOID*)&m_pWebBrowser2);

		if (!m_sPendingUrl.IsEmpty()) {
			Navigate2(m_sPendingUrl);
			m_sPendingUrl.Empty();
		}
		else if ( m_bAutoNavi && !m_sHomePage.IsEmpty())
		{
			this->Navigate2(m_sHomePage);
		}
		RegisterEventHandler(TRUE);
		return true;
	}

	void CWebBrowserUI::ReleaseControl()
	{
		m_bCreated = false;
		if (m_pManager != NULL) {
			m_pManager->RemoveTranslateAccelerator(this);
			m_pManager->RemoveMessageFilter(this);
		}
#if defined(DUILIB_USE_WEBVIEW2)
		if (m_bUseWebView2) {
			ReleaseWebView2();
			m_bUseWebView2 = false;
			return;
		}
#endif
		RegisterEventHandler(FALSE);
		CActiveXUI::ReleaseControl();
	}

	CWebBrowserUI::~CWebBrowserUI()
	{
		ReleaseControl();
	}

	STDMETHODIMP CWebBrowserUI::GetTypeInfoCount( UINT *iTInfo )
	{
		*iTInfo = 0;
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::GetTypeInfo( UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo )
	{
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::GetIDsOfNames( REFIID riid, OLECHAR **rgszNames, UINT cNames, LCID lcid,DISPID *rgDispId )
	{
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::Invoke( DISPID dispIdMember, REFIID riid, LCID lcid,WORD wFlags, DISPPARAMS* pDispParams,VARIANT* pVarResult, EXCEPINFO* pExcepInfo,UINT* puArgErr )
	{
		if ((riid != IID_NULL))
			return E_INVALIDARG;

		switch(dispIdMember)
		{
		case DISPID_BEFORENAVIGATE2:
			BeforeNavigate2(
				pDispParams->rgvarg[6].pdispVal,
				pDispParams->rgvarg[5].pvarVal,
				pDispParams->rgvarg[4].pvarVal,
				pDispParams->rgvarg[3].pvarVal,
				pDispParams->rgvarg[2].pvarVal,
				pDispParams->rgvarg[1].pvarVal,
				pDispParams->rgvarg[0].pboolVal);
			break;
		case DISPID_COMMANDSTATECHANGE:
			CommandStateChange(
				pDispParams->rgvarg[1].lVal,
				pDispParams->rgvarg[0].boolVal);
			break;
		case DISPID_NAVIGATECOMPLETE2:
			NavigateComplete2(
				pDispParams->rgvarg[1].pdispVal,
				pDispParams->rgvarg[0].pvarVal);
			break;
		case DISPID_NAVIGATEERROR:
			NavigateError(
				pDispParams->rgvarg[4].pdispVal,
				pDispParams->rgvarg[3].pvarVal,
				pDispParams->rgvarg[2].pvarVal,
				pDispParams->rgvarg[1].pvarVal,
				pDispParams->rgvarg[0].pboolVal);
			break;
		case DISPID_STATUSTEXTCHANGE:
			break;


		case DISPID_NEWWINDOW3:
			NewWindow3(
				pDispParams->rgvarg[4].ppdispVal,
				pDispParams->rgvarg[3].pboolVal,
				pDispParams->rgvarg[2].uintVal,
				pDispParams->rgvarg[1].bstrVal,
				pDispParams->rgvarg[0].bstrVal);
			break;
		case DISPID_TITLECHANGE:
			{
				TitleChange(pDispParams->rgvarg[0].bstrVal);
				break;
			}
		case DISPID_DOCUMENTCOMPLETE:
			{
				DocumentComplete(
					pDispParams->rgvarg[1].pdispVal,
					pDispParams->rgvarg[0].pvarVal);

				break;
			}
		default:
			return DISP_E_MEMBERNOTFOUND;
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::QueryInterface( REFIID riid, LPVOID *ppvObject )
	{
		*ppvObject = NULL;

		if( riid == IID_IDocHostUIHandler)
			*ppvObject = static_cast<IDocHostUIHandler*>(this);
		else if( riid == IID_IDispatch)
			*ppvObject = static_cast<IDispatch*>(this);
		else if( riid == IID_IServiceProvider)
			*ppvObject = static_cast<IServiceProvider*>(this);
		else if(riid == IID_IInternetSecurityManager ) {
			*ppvObject = static_cast<IInternetSecurityManager*>(this);
		}
		else if (riid == IID_IOleCommandTarget)
			*ppvObject = static_cast<IOleCommandTarget*>(this);

		if( *ppvObject != NULL )
			AddRef();
		return *ppvObject == NULL ? E_NOINTERFACE : S_OK;
	}

	STDMETHODIMP_(ULONG) CWebBrowserUI::AddRef()
	{
		InterlockedIncrement(&m_dwRef);
		return m_dwRef;
	}

	STDMETHODIMP_(ULONG) CWebBrowserUI::Release()
	{
		ULONG ulRefCount = InterlockedDecrement(&m_dwRef);
		return ulRefCount;
	}

	void CWebBrowserUI::Navigate2( LPCTSTR lpszUrl )
	{
		if (lpszUrl == NULL)
			return;


		if (!m_bCreated) {
			m_sPendingUrl = lpszUrl;
			return;
		}

#if defined(DUILIB_USE_WEBVIEW2)
		if (m_bUseWebView2)
		{
			if (m_pWebView2 != NULL)
				m_pWebView2->Navigate(CT2CW(lpszUrl));
			else
				m_sPendingUrl = lpszUrl;
			return;
		}
#endif
		if (m_pWebBrowser2)
		{
			CDuiVariant url;
			url.vt=VT_BSTR;
			url.bstrVal=T2BSTR(lpszUrl);
			HRESULT hr = m_pWebBrowser2->Navigate2(&url, NULL, NULL, NULL, NULL);
		}
	}

	void CWebBrowserUI::Refresh()
	{
#if defined(DUILIB_USE_WEBVIEW2)
		if (m_bUseWebView2) { if (m_pWebView2 != NULL) m_pWebView2->Reload(); return; }
#endif
		if (m_pWebBrowser2)
		{
			m_pWebBrowser2->Refresh();
		}
	}
	void CWebBrowserUI::GoBack()
	{
#if defined(DUILIB_USE_WEBVIEW2)
		if (m_bUseWebView2) { if (m_pWebView2 != NULL) m_pWebView2->GoBack(); return; }
#endif
		if (m_pWebBrowser2)
		{
			m_pWebBrowser2->GoBack();
		}
	}
	void CWebBrowserUI::GoForward()
	{
#if defined(DUILIB_USE_WEBVIEW2)
		if (m_bUseWebView2) { if (m_pWebView2 != NULL) m_pWebView2->GoForward(); return; }
#endif
		if (m_pWebBrowser2)
		{
			m_pWebBrowser2->GoForward();
		}
	}

	void CWebBrowserUI::BeforeNavigate2( IDispatch *pDisp,VARIANT *&url,VARIANT *&Flags,VARIANT *&TargetFrameName,VARIANT *&PostData,VARIANT *&Headers,VARIANT_BOOL *&Cancel )
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->BeforeNavigate2(this, pDisp,url,Flags,TargetFrameName,PostData,Headers,Cancel);
		}
	}

	void CWebBrowserUI::NavigateError( IDispatch *pDisp,VARIANT * &url,VARIANT *&TargetFrameName,VARIANT *&StatusCode,VARIANT_BOOL *&Cancel )
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->NavigateError(this, pDisp,url,TargetFrameName,StatusCode,Cancel);
		}
	}

	void CWebBrowserUI::NavigateComplete2( IDispatch *pDisp,VARIANT *&url )
	{
		CComPtr<IDispatch> spDoc;
		m_pWebBrowser2->get_Document(&spDoc);

		if (spDoc)
		{
			CComQIPtr<ICustomDoc, &IID_ICustomDoc> spCustomDoc(spDoc);
			if (spCustomDoc)
				spCustomDoc->SetUIHandler(this);
		}

		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->NavigateComplete2(this, pDisp,url);
		}
	}

	void CWebBrowserUI::ProgressChange( LONG nProgress, LONG nProgressMax )
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->ProgressChange(this, nProgress,nProgressMax);
		}
	}

	void CWebBrowserUI::NewWindow3( IDispatch **pDisp, VARIANT_BOOL *&Cancel, DWORD dwFlags, BSTR bstrUrlContext, BSTR bstrUrl )
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->NewWindow3(this, pDisp,Cancel,dwFlags,bstrUrlContext,bstrUrl);
		}
	}
	void CWebBrowserUI::CommandStateChange(long Command,VARIANT_BOOL Enable)
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->CommandStateChange(this, Command,Enable);
		}
	}

	void CWebBrowserUI::TitleChange(BSTR bstrTitle)
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->TitleChange(this, bstrTitle);
		}
	}

	void CWebBrowserUI::DocumentComplete(IDispatch *pDisp, VARIANT *&url)
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->DocumentComplete(this, pDisp, url);
		}
	}


	STDMETHODIMP CWebBrowserUI::Download(  IMoniker *pmk,  IBindCtx *pbc,  DWORD dwBindVerb,  LONG grfBINDF,  BINDINFO *pBindInfo,  LPCOLESTR pszHeaders,  LPCOLESTR pszRedir,  UINT uiCP )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->Download(this, pmk,pbc,dwBindVerb,grfBINDF,pBindInfo,pszHeaders,pszRedir,uiCP);
		}
		return S_OK;
	}


	STDMETHODIMP CWebBrowserUI::ShowContextMenu( DWORD dwID, POINT* pptPosition, IUnknown* pCommandTarget, IDispatch* pDispatchObjectHit )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->ShowContextMenu(this, dwID,pptPosition,pCommandTarget,pDispatchObjectHit);
		}
		return S_FALSE;
	}

	STDMETHODIMP CWebBrowserUI::GetHostInfo( DOCHOSTUIINFO* pInfo )
	{
		if (pInfo != NULL) {
			pInfo->dwFlags |= DOCHOSTUIFLAG_NO3DBORDER | DOCHOSTUIFLAG_NO3DOUTERBORDER;
		}
		if (m_pWebBrowserEventHandler) {
			return m_pWebBrowserEventHandler->GetHostInfo(this, pInfo);
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::ShowUI( DWORD dwID, IOleInPlaceActiveObject* pActiveObject, IOleCommandTarget* pCommandTarget, IOleInPlaceFrame* pFrame, IOleInPlaceUIWindow* pDoc )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->ShowUI(this, dwID,pActiveObject,pCommandTarget,pFrame,pDoc);
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::HideUI()
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->HideUI(this);
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::UpdateUI()
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->UpdateUI(this);
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::EnableModeless( BOOL fEnable )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->EnableModeless(this, fEnable);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::OnDocWindowActivate( BOOL fActivate )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->OnDocWindowActivate(this, fActivate);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::OnFrameWindowActivate( BOOL fActivate )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->OnFrameWindowActivate(this, fActivate);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::ResizeBorder( LPCRECT prcBorder, IOleInPlaceUIWindow* pUIWindow, BOOL fFrameWindow )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->ResizeBorder(this, prcBorder,pUIWindow,fFrameWindow);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::TranslateAccelerator( LPMSG lpMsg, const GUID* pguidCmdGroup, DWORD nCmdID )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->TranslateAccelerator(this, lpMsg,pguidCmdGroup,nCmdID);
		}
		return S_FALSE;
	}

	LRESULT CWebBrowserUI::TranslateAccelerator( MSG *pMsg )
	{
		if(pMsg->message < WM_KEYFIRST || pMsg->message > WM_KEYLAST)
			return S_FALSE;

		if( m_pWebBrowser2 == NULL )
			return E_NOTIMPL;


		BOOL bIsChild = FALSE;
		HWND hTempWnd = NULL;
		HWND hWndFocus = ::GetFocus();

		hTempWnd = hWndFocus;
		while(hTempWnd != NULL)
		{
			if(hTempWnd == m_hwndHost)
			{
				bIsChild = TRUE;
				break;
			}
			hTempWnd = ::GetParent(hTempWnd);
		}
		if(!bIsChild)
			return S_FALSE;

		IOleInPlaceActiveObject *pObj;
		if (FAILED(m_pWebBrowser2->QueryInterface(IID_IOleInPlaceActiveObject, (LPVOID *)&pObj)))
			return S_FALSE;

		HRESULT hResult = pObj->TranslateAccelerator(pMsg);
		pObj->Release();
		return hResult;
	}

	STDMETHODIMP CWebBrowserUI::GetOptionKeyPath( LPOLESTR* pchKey, DWORD dwReserved )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->GetOptionKeyPath(this, pchKey,dwReserved);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::GetDropTarget( IDropTarget* pDropTarget, IDropTarget** ppDropTarget )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->GetDropTarget(this, pDropTarget,ppDropTarget);
		}
		return S_FALSE;
	}

	STDMETHODIMP CWebBrowserUI::GetExternal( IDispatch** ppDispatch )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->GetExternal(this, ppDispatch);
		}
		return S_FALSE;
	}

	STDMETHODIMP CWebBrowserUI::TranslateUrl( DWORD dwTranslate, OLECHAR* pchURLIn, OLECHAR** ppchURLOut )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->TranslateUrl(this, dwTranslate,pchURLIn,ppchURLOut);
		}
		else
		{
			*ppchURLOut = 0;
			return E_NOTIMPL;
		}
	}

	STDMETHODIMP CWebBrowserUI::FilterDataObject( IDataObject* pDO, IDataObject** ppDORet )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->FilterDataObject(this, pDO,ppDORet);
		}
		else
		{
			*ppDORet = 0;
			return E_NOTIMPL;
		}
	}

	void CWebBrowserUI::SetWebBrowserEventHandler( CWebBrowserEventHandler* pEventHandler )
	{
		if ( pEventHandler!=NULL && m_pWebBrowserEventHandler!=pEventHandler)
			m_pWebBrowserEventHandler=pEventHandler;
	}

	void CWebBrowserUI::Refresh2( int Level )
	{
#if defined(DUILIB_USE_WEBVIEW2)

		if (m_bUseWebView2) { if (m_pWebView2 != NULL) m_pWebView2->Reload(); return; }
#endif
		CDuiVariant vLevel;
		vLevel.vt=VT_I4;
		vLevel.intVal=Level;
		m_pWebBrowser2->Refresh2(&vLevel);
	}

	void CWebBrowserUI::SetAttribute( LPCTSTR pstrName, LPCTSTR pstrValue )
	{
		if (_tcsicmp(pstrName, _T("homepage")) == 0)
		{
			m_sHomePage = pstrValue;
		}
		else if (_tcsicmp(pstrName, _T("autonavi"))==0)
		{
			m_bAutoNavi = (_tcsicmp(pstrValue, _T("true")) == 0);
		}
		else if (_tcsicmp(pstrName, _T("webview2"))==0)
		{

#if defined(DUILIB_USE_WEBVIEW2)
			SetWebView2Enabled(_tcsicmp(pstrValue, _T("false")) != 0);
#endif
		}
		else
			CActiveXUI::SetAttribute(pstrName, pstrValue);
	}

	void CWebBrowserUI::NavigateHomePage()
	{
		if (!m_sHomePage.IsEmpty())
			this->NavigateUrl(m_sHomePage);
	}

	void CWebBrowserUI::NavigateUrl( LPCTSTR lpszUrl )
	{

		Navigate2(lpszUrl);
	}

	LPCTSTR CWebBrowserUI::GetClass() const
	{
		return _T("WebBrowserUI");
	}

	LPVOID CWebBrowserUI::GetInterface( LPCTSTR pstrName )
	{
		if( _tcsicmp(pstrName, DUI_CTR_WEBBROWSER) == 0 ) return static_cast<CWebBrowserUI*>(this);
		return CActiveXUI::GetInterface(pstrName);
	}

	void CWebBrowserUI::SetHomePage( LPCTSTR lpszUrl )
	{
		m_sHomePage.Format(_T("%s"),lpszUrl);
	}

	LPCTSTR CWebBrowserUI::GetHomePage()
	{
		return m_sHomePage;
	}

	void CWebBrowserUI::SetAutoNavigation( bool bAuto  )
	{
		if (m_bAutoNavi==bAuto)	return;

		m_bAutoNavi=bAuto;
	}

	bool CWebBrowserUI::IsAutoNavigation()
	{
		return m_bAutoNavi;
	}

	STDMETHODIMP CWebBrowserUI::QueryService( REFGUID guidService, REFIID riid, void** ppvObject )
	{
		HRESULT hr = E_NOINTERFACE;
		*ppvObject = NULL;

		if ( guidService == SID_SDownloadManager && riid == IID_IDownloadManager)
		{
			*ppvObject = this;
			return S_OK;
		}
		if(guidService == SID_SInternetSecurityManager && riid == IID_IInternetSecurityManager) {
			*ppvObject = this;
			return S_OK;
		}
		return hr;
	}

	HRESULT CWebBrowserUI::RegisterEventHandler( BOOL inAdvise )
	{
		CComPtr<IWebBrowser2> pWebBrowser;
		CComPtr<IConnectionPointContainer>  pCPC;
		CComPtr<IConnectionPoint> pCP;
		HRESULT hr = GetControl(IID_IWebBrowser2, (void**)&pWebBrowser);
		if (FAILED(hr))
			return hr;
		hr=pWebBrowser->QueryInterface(IID_IConnectionPointContainer,(void **)&pCPC);
		if (FAILED(hr))
			return hr;
		hr=pCPC->FindConnectionPoint(DIID_DWebBrowserEvents2,&pCP);
		if (FAILED(hr))
			return hr;

		if (inAdvise)
		{
			hr = pCP->Advise((IDispatch*)this, &m_dwCookie);
		}
		else
		{
			hr = pCP->Unadvise(m_dwCookie);
		}
		return hr;
	}

	DISPID CWebBrowserUI::FindId( IDispatch *pObj, LPOLESTR pName )
	{
		DISPID id = 0;
		if(FAILED(pObj->GetIDsOfNames(IID_NULL,&pName,1,LOCALE_SYSTEM_DEFAULT,&id))) id = -1;
		return id;
	}

	HRESULT CWebBrowserUI::InvokeMethod( IDispatch *pObj, LPOLESTR pMehtod, VARIANT *pVarResult, VARIANT *ps, int cArgs )
	{
		DISPID dispid = FindId(pObj, pMehtod);
		if(dispid == -1) return E_FAIL;

		DISPPARAMS dispparams;
		dispparams.cArgs = cArgs;
		dispparams.rgvarg = ps;
		dispparams.cNamedArgs = 0;
		dispparams.rgdispidNamedArgs = NULL;

		return pObj->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD, &dispparams, pVarResult, NULL, NULL);
	}

	HRESULT CWebBrowserUI::GetProperty( IDispatch *pObj, LPOLESTR pName, VARIANT *pValue )
	{
		DISPID dispid = FindId(pObj, pName);
		if(dispid == -1) return E_FAIL;

		DISPPARAMS ps;
		ps.cArgs = 0;
		ps.rgvarg = NULL;
		ps.cNamedArgs = 0;
		ps.rgdispidNamedArgs = NULL;

		return pObj->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYGET, &ps, pValue, NULL, NULL);
	}

	HRESULT CWebBrowserUI::SetProperty( IDispatch *pObj, LPOLESTR pName, VARIANT *pValue )
	{
		DISPID dispid = FindId(pObj, pName);
		if(dispid == -1) return E_FAIL;

		DISPPARAMS ps;
		ps.cArgs = 1;
		ps.rgvarg = pValue;
		ps.cNamedArgs = 0;
		ps.rgdispidNamedArgs = NULL;

		return pObj->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYPUT, &ps, NULL, NULL, NULL);
	}

	IDispatch* CWebBrowserUI::GetHtmlWindow()
	{
		IDispatch* pDp =  NULL;
		HRESULT hr = E_FAIL;
		if (m_pWebBrowser2)
			hr=m_pWebBrowser2->get_Document(&pDp);

		if (FAILED(hr))
			return NULL;

		CComQIPtr<IHTMLDocument2> pHtmlDoc2 = pDp;

		if (pHtmlDoc2 == NULL)
			return NULL;

		hr=pHtmlDoc2->get_parentWindow(&_pHtmlWnd2);

		if (FAILED(hr))
			return NULL;

		IDispatch *pHtmlWindown = NULL;
		hr=_pHtmlWnd2->QueryInterface(IID_IDispatch, (void**)&pHtmlWindown);
		if (FAILED(hr))
			return NULL;

		return pHtmlWindown;
	}

	IWebBrowser2* CWebBrowserUI::GetWebBrowser2( void )
	{
		return m_pWebBrowser2;
	}

	HRESULT STDMETHODCALLTYPE CWebBrowserUI::QueryStatus( __RPC__in_opt const GUID *pguidCmdGroup, ULONG cCmds, __RPC__inout_ecount_full(cCmds ) OLECMD prgCmds[ ], __RPC__inout_opt OLECMDTEXT *pCmdText )
	{
		HRESULT hr = OLECMDERR_E_NOTSUPPORTED;
		return hr;
	}

	HRESULT STDMETHODCALLTYPE CWebBrowserUI::Exec( __RPC__in_opt const GUID *pguidCmdGroup, DWORD nCmdID, DWORD nCmdexecopt, __RPC__in_opt VARIANT *pvaIn, __RPC__inout_opt VARIANT *pvaOut )
	{
		HRESULT hr = S_OK;

		if (pguidCmdGroup && IsEqualGUID(*pguidCmdGroup, CGID_DocHostCommandHandler))
		{

			switch (nCmdID)
			{

			case OLECMDID_SHOWSCRIPTERROR:
				{
					IHTMLDocument2*             pDoc = NULL;
					IHTMLWindow2*               pWindow = NULL;
					IHTMLEventObj*              pEventObj = NULL;
					BSTR                        rgwszNames[5] =
					{
						SysAllocString(L"errorLine"),
						SysAllocString(L"errorCharacter"),
						SysAllocString(L"errorCode"),
						SysAllocString(L"errorMessage"),
						SysAllocString(L"errorUrl")
					};
					DISPID                      rgDispIDs[5];
					VARIANT                     rgvaEventInfo[5];
					DISPPARAMS                  params;
					BOOL                        fContinueRunningScripts = true;
					int                         i;

					params.cArgs = 0;
					params.cNamedArgs = 0;


					hr = pvaIn->punkVal->QueryInterface(IID_IHTMLDocument2, (void **) &pDoc);

					hr = pDoc->get_parentWindow(&pWindow);
					pDoc->Release();

					hr = pWindow->get_event(&pEventObj);

					for (i = 0; i < 5; i++)
					{

						hr = pEventObj->GetIDsOfNames(IID_NULL, &rgwszNames[i], 1,
							LOCALE_SYSTEM_DEFAULT, &rgDispIDs[i]);

						hr = pEventObj->Invoke(rgDispIDs[i], IID_NULL,
							LOCALE_SYSTEM_DEFAULT,
							DISPATCH_PROPERTYGET, &params, &rgvaEventInfo[i],
							NULL, NULL);
						SysFreeString(rgwszNames[i]);
					}





					(*pvaOut).vt = VT_BOOL;
					if (fContinueRunningScripts)
					{

						(*pvaOut).boolVal = VARIANT_TRUE;
					}
					else
					{

						(*pvaOut).boolVal = VARIANT_FALSE;
					}
					break;
				}
			default:
				hr = OLECMDERR_E_NOTSUPPORTED;
				break;
			}
		}
		else
		{
			hr = OLECMDERR_E_UNKNOWNGROUP;
		}
		return (hr);
	}

#if defined(DUILIB_USE_WEBVIEW2)





	namespace
	{

		template <typename T>
		class CWebView2SinkBase : public T
		{
		public:
			CWebView2SinkBase(HWND hNotifyWnd) : m_hNotifyWnd(hNotifyWnd), m_dwRef(1) {}
			virtual ~CWebView2SinkBase() {}

			STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override
			{
				if (riid == IID_IUnknown || riid == __uuidof(T)) {
					*ppvObject = static_cast<T*>(this);
					AddRef();
					return S_OK;
				}
				*ppvObject = NULL;
				return E_NOINTERFACE;
			}
			STDMETHOD_(ULONG, AddRef)() override { return ::InterlockedIncrement(&m_dwRef); }
			STDMETHOD_(ULONG, Release)() override
			{
				ULONG ulRef = ::InterlockedDecrement(&m_dwRef);
				if (ulRef == 0) delete this;
				return ulRef;
			}
		protected:
			HWND m_hNotifyWnd;
		private:
			LONG m_dwRef;
		};


		class CWebView2EnvCreatedSink
			: public CWebView2SinkBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
		{
		public:
			CWebView2EnvCreatedSink(HWND hNotifyWnd, UINT uMsg)
				: CWebView2SinkBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(hNotifyWnd), m_uMsg(uMsg) {}
			STDMETHOD(Invoke)(HRESULT result, ICoreWebView2Environment* pEnv) override
			{
				if (SUCCEEDED(result) && pEnv != NULL) {
					pEnv->AddRef();
					if (!::PostMessage(m_hNotifyWnd, m_uMsg, CWebBrowserUI::WEBVIEW2_MSG_CREATE_CONTROLLER, (LPARAM)pEnv))
						pEnv->Release();
				}
				else {
					::PostMessage(m_hNotifyWnd, m_uMsg, CWebBrowserUI::WEBVIEW2_MSG_FAILED, 0);
				}
				return S_OK;
			}
		private:
			UINT m_uMsg;
		};


		class CWebView2CtrlCreatedSink
			: public CWebView2SinkBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
		{
		public:
			CWebView2CtrlCreatedSink(HWND hNotifyWnd, UINT uMsg)
				: CWebView2SinkBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(hNotifyWnd), m_uMsg(uMsg) {}
			STDMETHOD(Invoke)(HRESULT result, ICoreWebView2Controller* pController) override
			{
				if (SUCCEEDED(result) && pController != NULL) {
					pController->AddRef();
					if (!::PostMessage(m_hNotifyWnd, m_uMsg, CWebBrowserUI::WEBVIEW2_MSG_BIND, (LPARAM)pController))
						pController->Release();
				}
				else {
					::PostMessage(m_hNotifyWnd, m_uMsg, CWebBrowserUI::WEBVIEW2_MSG_FAILED, 0);
				}
				return S_OK;
			}
		private:
			UINT m_uMsg;
		};


		class CWebView2NavStartingSink
			: public CWebView2SinkBase<ICoreWebView2NavigationStartingEventHandler>
		{
		public:
			CWebView2NavStartingSink(CWebBrowserUI* pOwner, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2NavigationStartingEventHandler>(hNotifyWnd), m_pOwner(pOwner) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, ICoreWebView2NavigationStartingEventArgs* pArgs) override
			{ if (m_pOwner != NULL) m_pOwner->OnWebView2NavigationStarting(pWebView, pArgs); return S_OK; }
		private:
			CWebBrowserUI* m_pOwner;
		};

		class CWebView2NavCompletedSink
			: public CWebView2SinkBase<ICoreWebView2NavigationCompletedEventHandler>
		{
		public:
			CWebView2NavCompletedSink(CWebBrowserUI* pOwner, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2NavigationCompletedEventHandler>(hNotifyWnd), m_pOwner(pOwner) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, ICoreWebView2NavigationCompletedEventArgs* pArgs) override
			{ if (m_pOwner != NULL) m_pOwner->OnWebView2NavigationCompleted(pWebView, pArgs); return S_OK; }
		private:
			CWebBrowserUI* m_pOwner;
		};

		class CWebView2TitleChangedSink
			: public CWebView2SinkBase<ICoreWebView2DocumentTitleChangedEventHandler>
		{
		public:
			CWebView2TitleChangedSink(CWebBrowserUI* pOwner, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2DocumentTitleChangedEventHandler>(hNotifyWnd), m_pOwner(pOwner) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, IUnknown* pArgs) override
			{ if (m_pOwner != NULL) m_pOwner->OnWebView2DocumentTitleChanged(pWebView); return S_OK; }
		private:
			CWebBrowserUI* m_pOwner;
		};

		class CWebView2NewWindowSink
			: public CWebView2SinkBase<ICoreWebView2NewWindowRequestedEventHandler>
		{
		public:
			CWebView2NewWindowSink(CWebBrowserUI* pOwner, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2NewWindowRequestedEventHandler>(hNotifyWnd), m_pOwner(pOwner) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, ICoreWebView2NewWindowRequestedEventArgs* pArgs) override
			{ if (m_pOwner != NULL) m_pOwner->OnWebView2NewWindowRequested(pWebView, pArgs); return S_OK; }
		private:
			CWebBrowserUI* m_pOwner;
		};

		class CWebView2HistoryChangedSink
			: public CWebView2SinkBase<ICoreWebView2HistoryChangedEventHandler>
		{
		public:
			CWebView2HistoryChangedSink(CWebBrowserUI* pOwner, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2HistoryChangedEventHandler>(hNotifyWnd), m_pOwner(pOwner) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, IUnknown* pArgs) override
			{ if (m_pOwner != NULL) m_pOwner->OnWebView2HistoryChanged(pWebView); return S_OK; }
		private:
			CWebBrowserUI* m_pOwner;
		};

		class CWebView2WebMessageSink
			: public CWebView2SinkBase<ICoreWebView2WebMessageReceivedEventHandler>
		{
		public:
			CWebView2WebMessageSink(CWebBrowserUI* pOwner, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2WebMessageReceivedEventHandler>(hNotifyWnd), m_pOwner(pOwner) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, ICoreWebView2WebMessageReceivedEventArgs* pArgs) override
			{ if (m_pOwner != NULL) m_pOwner->OnWebView2WebMessageReceived(pWebView, pArgs); return S_OK; }
		private:
			CWebBrowserUI* m_pOwner;
		};
	}

	UINT CWebBrowserUI::GetWebView2NotifyMessage()
	{
		static UINT s_uMsg = ::RegisterWindowMessage(_T("DuiLib_WebView2_Notify"));
		return s_uMsg;
	}

	bool CWebBrowserUI::IsWebView2Mode() const
	{
		return m_bUseWebView2;
	}

	void CWebBrowserUI::SetWebView2Enabled(bool bEnable)
	{

		m_nWebView2Mode = bEnable ? 1 : 0;
	}

	bool CWebBrowserUI::IsWebView2Supported()
	{

		if (m_pManager != NULL && m_pManager->IsLayered()) return false;
		return WebView2Helper::IsWebView2Supported();
	}

	ICoreWebView2* CWebBrowserUI::GetWebView2() const
	{
		return m_pWebView2;
	}

	void CWebBrowserUI::Stop()
	{
		if (m_bUseWebView2) {
			if (m_pWebView2 != NULL) m_pWebView2->Stop();
			return;
		}
		if (m_pWebBrowser2) m_pWebBrowser2->Stop();
	}

	void CWebBrowserUI::PostWebMessage(LPCTSTR lpszMessage)
	{
		if (!m_bUseWebView2 || lpszMessage == NULL) return;
		if (m_pWebView2 != NULL)
			m_pWebView2->PostWebMessageAsString(lpszMessage);
	}

	bool CWebBrowserUI::InitWebView2()
	{
		WebView2Helper::PFN_CreateCoreWebView2EnvironmentWithOptions pfnCreateEnv = WebView2Helper::GetCreateEnvironmentFunc();
		if (pfnCreateEnv == NULL) return false;

		HWND hNotifyWnd = (m_pManager != NULL) ? m_pManager->GetPaintWindow() : NULL;
		if (hNotifyWnd == NULL) return false;

		CWebView2EnvCreatedSink* pSink = new CWebView2EnvCreatedSink(hNotifyWnd, m_uWebView2NotifyMsg);
		HRESULT hr = pfnCreateEnv(NULL, NULL, NULL, (IUnknown*)pSink);
		pSink->Release();
		if (FAILED(hr)) return false;
		return true;
	}

	void CWebBrowserUI::ReleaseWebView2()
	{
		if (m_pWebView2 != NULL) { m_pWebView2->Release(); m_pWebView2 = NULL; }
		if (m_pWebView2Controller != NULL) {
			m_pWebView2Controller->Close();
			m_pWebView2Controller->Release();
			m_pWebView2Controller = NULL;
		}
		if (m_pWebView2Env != NULL) { m_pWebView2Env->Release(); m_pWebView2Env = NULL; }
		m_sPendingUrl.Empty();
	}

	void CWebBrowserUI::UpdateWebView2Bounds()
	{
		if (m_pWebView2Controller == NULL) return;
		m_pWebView2Controller->put_Bounds(m_rcItem);
	}

	void CWebBrowserUI::OnWebView2Notify(WPARAM wParam, LPARAM lParam)
	{
		switch (wParam)
		{
		case WEBVIEW2_MSG_CREATE_CONTROLLER:
			{
				ICoreWebView2Environment* pEnv = (ICoreWebView2Environment*)lParam;
				if (pEnv == NULL) return;
				if (m_pWebView2Env != NULL) m_pWebView2Env->Release();
				m_pWebView2Env = pEnv;

				HWND hNotifyWnd = (m_pManager != NULL) ? m_pManager->GetPaintWindow() : NULL;
				if (hNotifyWnd == NULL) return;
				CWebView2CtrlCreatedSink* pSink = new CWebView2CtrlCreatedSink(hNotifyWnd, m_uWebView2NotifyMsg);
				HRESULT hr = m_pWebView2Env->CreateCoreWebView2Controller(hNotifyWnd, pSink);
				pSink->Release();
				if (FAILED(hr)) { ReleaseWebView2(); m_bUseWebView2 = false; DoCreateActiveXBrowser(); }
				break;
			}
		case WEBVIEW2_MSG_BIND:
			{
				ICoreWebView2Controller* pController = (ICoreWebView2Controller*)lParam;
				if (pController == NULL) return;
				if (m_pWebView2Controller != NULL) m_pWebView2Controller->Release();
				m_pWebView2Controller = pController;
				OnWebView2Bind();
				break;
			}
		case WEBVIEW2_MSG_FAILED:
			{

				ReleaseWebView2();
				m_bUseWebView2 = false;
				DoCreateActiveXBrowser();
				break;
			}
		default:
			break;
		}
	}

	void CWebBrowserUI::OnWebView2Bind()
	{
		if (m_pWebView2Controller == NULL || m_pWebView2 != NULL) return;
		if (FAILED(m_pWebView2Controller->get_CoreWebView2(&m_pWebView2))) return;

		HWND hNotifyWnd = (m_pManager != NULL) ? m_pManager->GetPaintWindow() : NULL;

		EventRegistrationToken token;
		CWebView2NavStartingSink* pNavStart = new CWebView2NavStartingSink(this, hNotifyWnd);
		m_pWebView2->add_NavigationStarting(pNavStart, &token);
		pNavStart->Release();
		CWebView2NavCompletedSink* pNavDone = new CWebView2NavCompletedSink(this, hNotifyWnd);
		m_pWebView2->add_NavigationCompleted(pNavDone, &token);
		pNavDone->Release();
		CWebView2TitleChangedSink* pTitle = new CWebView2TitleChangedSink(this, hNotifyWnd);
		m_pWebView2->add_DocumentTitleChanged(pTitle, &token);
		pTitle->Release();
		CWebView2NewWindowSink* pNewWin = new CWebView2NewWindowSink(this, hNotifyWnd);
		m_pWebView2->add_NewWindowRequested(pNewWin, &token);
		pNewWin->Release();
		CWebView2HistoryChangedSink* pHistory = new CWebView2HistoryChangedSink(this, hNotifyWnd);
		m_pWebView2->add_HistoryChanged(pHistory, &token);
		pHistory->Release();
		CWebView2WebMessageSink* pMsg = new CWebView2WebMessageSink(this, hNotifyWnd);
		m_pWebView2->add_WebMessageReceived(pMsg, &token);
		pMsg->Release();

		UpdateWebView2Bounds();
		m_pWebView2Controller->put_IsVisible(IsVisible() ? TRUE : FALSE);


		if (!m_sPendingUrl.IsEmpty()) {
			Navigate2(m_sPendingUrl);
			m_sPendingUrl.Empty();
		}
		else if (m_bAutoNavi && !m_sHomePage.IsEmpty()) {
			Navigate2(m_sHomePage);
		}
	}





	void CWebBrowserUI::SetPos(RECT rc, bool bNeedInvalidate)
	{
		if (m_bUseWebView2) {
			CControlUI::SetPos(rc, bNeedInvalidate);
			if (!m_bCreated) DoCreateControl();
			UpdateWebView2Bounds();
			return;
		}
		CActiveXUI::SetPos(rc, bNeedInvalidate);
	}

	void CWebBrowserUI::Move(SIZE szOffset, bool bNeedInvalidate)
	{
		if (m_bUseWebView2) {
			CControlUI::Move(szOffset, bNeedInvalidate);
			UpdateWebView2Bounds();
			return;
		}
		CActiveXUI::Move(szOffset, bNeedInvalidate);
	}

	void CWebBrowserUI::SetVisible(bool bVisible)
	{
		if (m_bUseWebView2) {
			CControlUI::SetVisible(bVisible);
			if (m_pWebView2Controller != NULL)
				m_pWebView2Controller->put_IsVisible(IsVisible() ? TRUE : FALSE);
			return;
		}
		CActiveXUI::SetVisible(bVisible);
	}

	void CWebBrowserUI::SetInternVisible(bool bVisible)
	{
		if (m_bUseWebView2) {
			CControlUI::SetInternVisible(bVisible);
			if (m_pWebView2Controller != NULL)
				m_pWebView2Controller->put_IsVisible(IsVisible() ? TRUE : FALSE);
			return;
		}
		CActiveXUI::SetInternVisible(bVisible);
	}

	LRESULT CWebBrowserUI::MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam, bool& bHandled)
	{
		if (m_bUseWebView2) {

			if (uMsg == m_uWebView2NotifyMsg) {
				OnWebView2Notify(wParam, lParam);
				bHandled = true;
				return 0;
			}
			return 0;
		}
		return CActiveXUI::MessageHandler(uMsg, wParam, lParam, bHandled);
	}





	void CWebBrowserUI::OnWebView2NavigationStarting(ICoreWebView2* pWebView, ICoreWebView2NavigationStartingEventArgs* pArgs)
	{
		if (m_pWebBrowserEventHandler == NULL || pArgs == NULL) return;

		LPWSTR lpszUri = NULL;
		pArgs->get_Uri(&lpszUri);

		VARIANT vUrl; VariantInit(&vUrl);
		vUrl.vt = VT_BSTR;
		vUrl.bstrVal = SysAllocString(lpszUri != NULL ? lpszUri : L"");
		VARIANT vFlags; VariantInit(&vFlags); vFlags.vt = VT_I4; vFlags.lVal = 0;
		VARIANT vFrame; VariantInit(&vFrame); vFrame.vt = VT_BSTR; vFrame.bstrVal = NULL;
		VARIANT vPost; VariantInit(&vPost); vPost.vt = VT_EMPTY;
		VARIANT vHeaders; VariantInit(&vHeaders); vHeaders.vt = VT_EMPTY;
		VARIANT* pUrl = &vUrl;
		VARIANT* pFlags = &vFlags;
		VARIANT* pFrame = &vFrame;
		VARIANT* pPost = &vPost;
		VARIANT* pHeaders = &vHeaders;
		VARIANT_BOOL bCancel = VARIANT_FALSE;
		VARIANT_BOOL* pCancel = &bCancel;

		m_pWebBrowserEventHandler->BeforeNavigate2(this, NULL, pUrl, pFlags, pFrame, pPost, pHeaders, pCancel);
		if (bCancel == VARIANT_TRUE) {
			BOOL bCancelled = TRUE;
			pArgs->put_Cancel(bCancelled);
		}
		VariantClear(&vUrl);
		if (lpszUri != NULL) ::CoTaskMemFree(lpszUri);
	}

	void CWebBrowserUI::OnWebView2NavigationCompleted(ICoreWebView2* pWebView, ICoreWebView2NavigationCompletedEventArgs* pArgs)
	{
		if (m_pWebBrowserEventHandler == NULL || pArgs == NULL) return;

		BOOL bIsSuccess = FALSE;
		pArgs->get_IsSuccess(&bIsSuccess);

		LPWSTR lpszSource = NULL;
		if (pWebView != NULL) pWebView->get_Source(&lpszSource);
		VARIANT vUrl; VariantInit(&vUrl);
		vUrl.vt = VT_BSTR;
		vUrl.bstrVal = SysAllocString(lpszSource != NULL ? lpszSource : L"");
		VARIANT* pUrl = &vUrl;

		if (bIsSuccess) {
			m_pWebBrowserEventHandler->NavigateComplete2(this, NULL, pUrl);
			m_pWebBrowserEventHandler->DocumentComplete(this, NULL, pUrl);
		}
		else {
			COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
			pArgs->get_WebErrorStatus(&status);
			VARIANT vFrame; VariantInit(&vFrame); vFrame.vt = VT_EMPTY;
			VARIANT vStatus; VariantInit(&vStatus); vStatus.vt = VT_I4; vStatus.lVal = (LONG)status;
			VARIANT* pFrame = &vFrame;
			VARIANT* pStatus = &vStatus;
			VARIANT_BOOL bCancel = VARIANT_FALSE;
			VARIANT_BOOL* pCancel = &bCancel;
			m_pWebBrowserEventHandler->NavigateError(this, NULL, pUrl, pFrame, pStatus, pCancel);
		}
		VariantClear(&vUrl);
		if (lpszSource != NULL) ::CoTaskMemFree(lpszSource);
	}

	void CWebBrowserUI::OnWebView2DocumentTitleChanged(ICoreWebView2* pWebView)
	{
		if (m_pWebBrowserEventHandler == NULL || pWebView == NULL) return;
		LPWSTR lpszTitle = NULL;
		if (SUCCEEDED(pWebView->get_DocumentTitle(&lpszTitle)) && lpszTitle != NULL) {
			m_pWebBrowserEventHandler->TitleChange(this, lpszTitle);
			::CoTaskMemFree(lpszTitle);
		}
	}

	void CWebBrowserUI::OnWebView2NewWindowRequested(ICoreWebView2* pWebView, ICoreWebView2NewWindowRequestedEventArgs* pArgs)
	{
		if (pArgs == NULL) return;

		LPWSTR lpszUri = NULL;
		pArgs->get_Uri(&lpszUri);

		VARIANT_BOOL bCancel = VARIANT_FALSE;
		if (m_pWebBrowserEventHandler != NULL) {
			VARIANT_BOOL* pCancel = &bCancel;
			m_pWebBrowserEventHandler->NewWindow3(this, NULL, pCancel, 0, NULL, lpszUri);
		}

		BOOL bHandled = TRUE;
		pArgs->put_Handled(bHandled);
		if (bCancel != VARIANT_TRUE && m_pWebView2 != NULL && lpszUri != NULL)
			m_pWebView2->Navigate(lpszUri);

		if (lpszUri != NULL) ::CoTaskMemFree(lpszUri);
	}

	void CWebBrowserUI::OnWebView2HistoryChanged(ICoreWebView2* pWebView)
	{
		if (m_pWebBrowserEventHandler == NULL || pWebView == NULL) return;
		BOOL bCanBack = FALSE, bCanForward = FALSE;
		pWebView->get_CanGoBack(&bCanBack);
		pWebView->get_CanGoForward(&bCanForward);

		m_pWebBrowserEventHandler->CommandStateChange(this, 1, bCanBack ? VARIANT_TRUE : VARIANT_FALSE);
		m_pWebBrowserEventHandler->CommandStateChange(this, 2, bCanForward ? VARIANT_TRUE : VARIANT_FALSE);
	}

	void CWebBrowserUI::OnWebView2WebMessageReceived(ICoreWebView2* pWebView, ICoreWebView2WebMessageReceivedEventArgs* pArgs)
	{
		if (m_pWebBrowserEventHandler == NULL || pArgs == NULL) return;
		LPWSTR lpszMessage = NULL;
		if (SUCCEEDED(pArgs->get_WebMessageAsJson(&lpszMessage)) && lpszMessage != NULL) {
			m_pWebBrowserEventHandler->WebMessageReceived(this, lpszMessage);
			::CoTaskMemFree(lpszMessage);
		}
	}
#endif
}

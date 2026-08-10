


#ifndef __DRAGDROPIMPL_H__
#define __DRAGDROPIMPL_H__
#include <shlobj.h>
#include <vector>

namespace DuiLib {


	typedef std::vector<FORMATETC> FormatEtcArray;
	typedef std::vector<FORMATETC*> PFormatEtcArray;
	typedef std::vector<STGMEDIUM*> PStgMediumArray;



	class UILIB_API CEnumFormatEtc : public IEnumFORMATETC
	{
	private:
		ULONG           m_cRefCount;
		FormatEtcArray  m_pFmtEtc;
		int           m_iCur;

	public:
		CEnumFormatEtc(const FormatEtcArray& ArrFE);
		CEnumFormatEtc(const PFormatEtcArray& ArrFE);

		STDMETHOD(QueryInterface)(REFIID, void FAR* FAR*);
		STDMETHOD_(ULONG, AddRef)(void);
		STDMETHOD_(ULONG, Release)(void);


		STDMETHOD(Next)(ULONG, LPFORMATETC, ULONG FAR *);
		STDMETHOD(Skip)(ULONG);
		STDMETHOD(Reset)(void);
		STDMETHOD(Clone)(IEnumFORMATETC FAR * FAR*);
	};



	class UILIB_API CIDropSource : public IDropSource
	{
		long m_cRefCount;
	public:
		bool m_bDropped;
		CIDropSource():m_cRefCount(0),m_bDropped(false) {}

		virtual HRESULT STDMETHODCALLTYPE QueryInterface(
			 REFIID riid,
			 void __RPC_FAR *__RPC_FAR *ppvObject);
		virtual ULONG STDMETHODCALLTYPE AddRef( void);
		virtual ULONG STDMETHODCALLTYPE Release( void);

		virtual HRESULT STDMETHODCALLTYPE QueryContinueDrag(
			 BOOL fEscapePressed,
			 DWORD grfKeyState);

		virtual HRESULT STDMETHODCALLTYPE GiveFeedback(
			 DWORD dwEffect);
	};



	class UILIB_API CIDataObject : public IDataObject
	{
		CIDropSource* m_pDropSource;
		long m_cRefCount;
		PFormatEtcArray m_ArrFormatEtc;
		PStgMediumArray m_StgMedium;

	public:
		CIDataObject(CIDropSource* pDropSource);
		~CIDataObject();
		void CopyMedium(STGMEDIUM* pMedDest, STGMEDIUM* pMedSrc, FORMATETC* pFmtSrc);

		virtual HRESULT STDMETHODCALLTYPE QueryInterface(
			 REFIID riid,
			 void __RPC_FAR *__RPC_FAR *ppvObject);
		virtual ULONG STDMETHODCALLTYPE AddRef( void);
		virtual ULONG STDMETHODCALLTYPE Release( void);


		virtual  HRESULT STDMETHODCALLTYPE GetData(
			 FORMATETC __RPC_FAR *pformatetcIn,
			 STGMEDIUM __RPC_FAR *pmedium);

		virtual  HRESULT STDMETHODCALLTYPE GetDataHere(
			 FORMATETC __RPC_FAR *pformatetc,
			 STGMEDIUM __RPC_FAR *pmedium);

		virtual HRESULT STDMETHODCALLTYPE QueryGetData(
			 FORMATETC __RPC_FAR *pformatetc);

		virtual HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
			 FORMATETC __RPC_FAR *pformatectIn,
			 FORMATETC __RPC_FAR *pformatetcOut);

		virtual  HRESULT STDMETHODCALLTYPE SetData(
			 FORMATETC __RPC_FAR *pformatetc,
			 STGMEDIUM __RPC_FAR *pmedium,
			 BOOL fRelease);

		virtual HRESULT STDMETHODCALLTYPE EnumFormatEtc(
			 DWORD dwDirection,
			 IEnumFORMATETC __RPC_FAR *__RPC_FAR *ppenumFormatEtc);

		virtual HRESULT STDMETHODCALLTYPE DAdvise(
			 FORMATETC __RPC_FAR *pformatetc,
			 DWORD advf,
			 IAdviseSink __RPC_FAR *pAdvSink,
			 DWORD __RPC_FAR *pdwConnection);

		virtual HRESULT STDMETHODCALLTYPE DUnadvise(
			 DWORD dwConnection);

		virtual HRESULT STDMETHODCALLTYPE EnumDAdvise(
			 IEnumSTATDATA __RPC_FAR *__RPC_FAR *ppenumAdvise);

































	};



	class UILIB_API CIDropTarget : public IDropTarget
	{
	public:
		CIDropTarget();
		virtual ~CIDropTarget();

	public:
		void SetTargetWnd(HWND hWnd) { m_hTargetWnd = hWnd; }
		void AddSuportedFormat(FORMATETC& ftetc) { m_formatetc.push_back(ftetc); }

	public:

		virtual bool OnDrop(FORMATETC* pFmtEtc, STGMEDIUM& medium,DWORD *pdwEffect) = 0;

		virtual HRESULT STDMETHODCALLTYPE QueryInterface(
			 REFIID riid,
			 void __RPC_FAR *__RPC_FAR *ppvObject);
		virtual ULONG STDMETHODCALLTYPE AddRef( void) { return ++m_cRefCount; }
		virtual ULONG STDMETHODCALLTYPE Release( void);

		bool QueryDrop(DWORD grfKeyState, LPDWORD pdwEffect);
		virtual HRESULT STDMETHODCALLTYPE DragEnter(
			 IDataObject __RPC_FAR *pDataObj,
			 DWORD grfKeyState,
			 POINTL pt,
			 DWORD __RPC_FAR *pdwEffect);
		virtual HRESULT STDMETHODCALLTYPE DragOver(
			 DWORD grfKeyState,
			 POINTL pt,
			 DWORD __RPC_FAR *pdwEffect);
		virtual HRESULT STDMETHODCALLTYPE DragLeave( void);
		virtual HRESULT STDMETHODCALLTYPE Drop(
			 IDataObject __RPC_FAR *pDataObj,
			 DWORD grfKeyState,
			 POINTL pt,
			 DWORD __RPC_FAR *pdwEffect);

	protected:
		HWND m_hTargetWnd;

	private:
		DWORD m_cRefCount;
		bool m_bAllowDrop;
		struct IDropTargetHelper *m_pDropTargetHelper;
		FormatEtcArray m_formatetc;
		FORMATETC* m_pSupportedFrmt;
	};



	class UILIB_API CDragSourceHelper
	{
	public:
		CDragSourceHelper()
		{
			m_pDragSourceHelper = NULL;
			CoCreateInstance(CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, IID_IDragSourceHelper, (void**)&m_pDragSourceHelper);
		}

		virtual ~CDragSourceHelper()
		{
			if( m_pDragSourceHelper!= NULL ) {
				m_pDragSourceHelper->Release();
				m_pDragSourceHelper=NULL;
			}
		}

	public:

		HRESULT InitializeFromBitmap(HBITMAP hBitmap,  POINT& pt, RECT& rc,	IDataObject* pDataObject, COLORREF crColorKey = GetSysColor(COLOR_WINDOW))
		{
			if(m_pDragSourceHelper == NULL) {
				return E_FAIL;
			}

			SHDRAGIMAGE di;
			BITMAP bm;
			GetObject(hBitmap, sizeof(bm), &bm);
			di.sizeDragImage.cx = bm.bmWidth;
			di.sizeDragImage.cy = bm.bmHeight;
			di.hbmpDragImage = hBitmap;
			di.crColorKey = crColorKey;
			di.ptOffset.x = pt.x - rc.left;
			di.ptOffset.y = pt.y - rc.top;
			return m_pDragSourceHelper->InitializeFromBitmap(&di, pDataObject);
		}

		HRESULT InitializeFromWindow(HWND hwnd, POINT& pt,IDataObject* pDataObject)
		{
			if(m_pDragSourceHelper == NULL) {
				return E_FAIL;
			}
			return m_pDragSourceHelper->InitializeFromWindow(hwnd, &pt, pDataObject);
		}

	private:
		IDragSourceHelper* m_pDragSourceHelper;
	};
}
#endif

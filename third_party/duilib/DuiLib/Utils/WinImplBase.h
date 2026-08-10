#ifndef WIN_IMPL_BASE_HPP
#define WIN_IMPL_BASE_HPP

namespace DuiLib
{
	class UILIB_API WindowImplBase
		: public CWindowWnd
		, public CNotifyPump
		, public INotifyUI
		, public IMessageFilterUI
		, public IDialogBuilderCallback
		, public IQueryControlText
	{
	public:
		WindowImplBase(){};
		virtual ~WindowImplBase(){};

		virtual void InitResource(){};

		virtual void InitWindow(){};
		virtual void OnFinalMessage( HWND hWnd );
		virtual void Notify(TNotifyUI& msg);

		DUI_DECLARE_MESSAGE_MAP()
		virtual void OnClick(TNotifyUI& msg);
		virtual BOOL IsInStaticControl(CControlUI *pControl);

	protected:
		virtual CDuiString GetSkinType() { return _T(""); }
		virtual CDuiString GetSkinFile() = 0;
		virtual LPCTSTR GetWindowClassName(void) const = 0 ;
		virtual LPCTSTR GetManagerName() { return NULL; }
		virtual LRESULT ResponseDefaultKeyEvent(WPARAM wParam);
		CPaintManagerUI m_pm;

	public:
		virtual UINT GetClassStyle() const;
		virtual CControlUI* CreateControl(LPCTSTR pstrClass);
		virtual LPCTSTR QueryControlText(LPCTSTR lpstrId, LPCTSTR lpstrType);

		virtual LRESULT MessageHandler(UINT uMsg, WPARAM wParam, LPARAM , bool& );
		virtual LRESULT OnClose(UINT , WPARAM , LPARAM , BOOL& bHandled);
		virtual LRESULT OnDestroy(UINT , WPARAM , LPARAM , BOOL& bHandled);

#if defined(WIN32) && !defined(UNDER_CE)
		virtual LRESULT OnNcActivate(UINT , WPARAM wParam, LPARAM , BOOL& bHandled);
		virtual LRESULT OnNcCalcSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
		virtual LRESULT OnNcPaint(UINT , WPARAM , LPARAM , BOOL& );
		virtual LRESULT OnNcHitTest(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
		virtual LRESULT OnGetMinMaxInfo(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
		virtual LRESULT OnMouseWheel(UINT , WPARAM , LPARAM , BOOL& bHandled);
		virtual LRESULT OnMouseHover(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
#endif
		virtual LRESULT OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
		virtual LRESULT OnChar(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
		virtual LRESULT OnSysCommand(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
		virtual LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
		virtual LRESULT OnKeyDown(UINT , WPARAM , LPARAM , BOOL& bHandled);
		virtual LRESULT OnKillFocus(UINT , WPARAM , LPARAM , BOOL& bHandled);
		virtual LRESULT OnSetFocus(UINT , WPARAM , LPARAM , BOOL& bHandled);
		virtual LRESULT OnLButtonDown(UINT , WPARAM , LPARAM , BOOL& bHandled);
		virtual LRESULT OnLButtonUp(UINT , WPARAM , LPARAM , BOOL& bHandled);
		virtual LRESULT OnMouseMove(UINT , WPARAM , LPARAM , BOOL& bHandled);
		virtual LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
		virtual LRESULT HandleCustomMessage(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
		virtual LONG GetStyle();
	};
}

#endif

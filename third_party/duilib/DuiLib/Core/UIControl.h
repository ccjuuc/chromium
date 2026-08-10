#ifndef __UICONTROL_H__
#define __UICONTROL_H__

#pragma once

namespace DuiLib {




	typedef CControlUI* (CALLBACK* FINDCONTROLPROC)(CControlUI*, LPVOID);

	class UILIB_API CControlUI
	{
		DECLARE_DUICONTROL(CControlUI)
	public:
		CControlUI();
		virtual ~CControlUI();

	public:
		virtual CDuiString GetName() const;
		virtual void SetName(LPCTSTR pstrName);
		virtual LPCTSTR GetClass() const;
		virtual LPVOID GetInterface(LPCTSTR pstrName);
		virtual UINT GetControlFlags() const;

		virtual bool Activate();
		virtual CPaintManagerUI* GetManager() const;
		virtual void SetManager(CPaintManagerUI* pManager, CControlUI* pParent, bool bInit = true);
		virtual CControlUI* GetParent() const;
	    void setInstance(HINSTANCE instance = NULL) {m_instance = instance;};


		bool SetTimer(UINT nTimerID, UINT nElapse);
		void KillTimer(UINT nTimerID);


		virtual CDuiString GetText() const;
		virtual void SetText(LPCTSTR pstrText);

		virtual bool IsResourceText() const;
		virtual void SetResourceText(bool bResource);

		virtual bool IsDragEnabled() const;
		virtual void SetDragEnable(bool bDrag);

		virtual bool IsDropEnabled() const;
		virtual void SetDropEnable(bool bDrop);

		virtual bool IsRichEvent() const;
		virtual void SetRichEvent(bool bEnable);

		LPCTSTR GetGradient();
		void SetGradient(LPCTSTR pStrImage);
		DWORD GetBkColor() const;
		void SetBkColor(DWORD dwBackColor);
		DWORD GetBkColor2() const;
		void SetBkColor2(DWORD dwBackColor);
		DWORD GetBkColor3() const;
		void SetBkColor3(DWORD dwBackColor);
		DWORD GetForeColor() const;
		void SetForeColor(DWORD dwForeColor);
		LPCTSTR GetBkImage();
		void SetBkImage(LPCTSTR pStrImage);
		LPCTSTR GetForeImage() const;
		void SetForeImage(LPCTSTR pStrImage);

		DWORD GetFocusBorderColor() const;
		void SetFocusBorderColor(DWORD dwBorderColor);
		bool IsColorHSL() const;
		void SetColorHSL(bool bColorHSL);
		SIZE GetBorderRound() const;
		void SetBorderRound(SIZE cxyRound);
		bool DrawImage(HDC hDC, LPCTSTR pStrImage, LPCTSTR pStrModify = NULL);


		int GetBorderSize() const;
		void SetBorderSize(int nSize);
		DWORD GetBorderColor() const;
		void SetBorderColor(DWORD dwBorderColor);
		RECT GetBorderRectSize() const;
		void SetBorderSize(RECT rc);
		int GetLeftBorderSize() const;
		void SetLeftBorderSize(int nSize);
		int GetTopBorderSize() const;
		void SetTopBorderSize(int nSize);
		int GetRightBorderSize() const;
		void SetRightBorderSize(int nSize);
		int GetBottomBorderSize() const;
		void SetBottomBorderSize(int nSize);
		int GetBorderStyle() const;
		void SetBorderStyle(int nStyle);


		virtual RECT GetRelativePos() const;
		virtual RECT GetClientPos() const;
		virtual const RECT& GetPos() const;
		virtual void SetPos(RECT rc, bool bNeedInvalidate = true);
		virtual void Move(SIZE szOffset, bool bNeedInvalidate = true);
		virtual int GetWidth() const;
		virtual int GetHeight() const;
		virtual int GetX() const;
		virtual int GetY() const;
		virtual RECT GetPadding() const;
		virtual void SetPadding(RECT rcPadding);
		virtual SIZE GetFixedXY() const;
		virtual void SetFixedXY(SIZE szXY);
		virtual SIZE GetFixedSize() const;
		virtual int GetFixedWidth() const;
		virtual void SetFixedWidth(int cx);
		virtual int GetFixedHeight() const;
		virtual void SetFixedHeight(int cy);
		virtual int GetMinWidth() const;
		virtual void SetMinWidth(int cx);
		virtual int GetMaxWidth() const;
		virtual void SetMaxWidth(int cx);
		virtual int GetMinHeight() const;
		virtual void SetMinHeight(int cy);
		virtual int GetMaxHeight() const;
		virtual void SetMaxHeight(int cy);
		virtual TPercentInfo GetFloatPercent() const;
		virtual void SetFloatPercent(TPercentInfo piFloatPercent);
		virtual void SetFloatAlign(UINT uAlign);
		virtual UINT GetFloatAlign() const;

		virtual CDuiString GetToolTip() const;
		virtual void SetToolTip(LPCTSTR pstrText);
		virtual void SetToolTipWidth(int nWidth);
		virtual int	  GetToolTipWidth(void);


		virtual WORD GetCursor();
		virtual void SetCursor(WORD wCursor);


		virtual TCHAR GetShortcut() const;
		virtual void SetShortcut(TCHAR ch);


		virtual bool IsContextMenuUsed() const;
		virtual void SetContextMenuUsed(bool bMenuUsed);


		virtual const CDuiString& GetUserData();
		virtual void SetUserData(LPCTSTR pstrText);
		virtual UINT_PTR GetTag() const;
		virtual void SetTag(UINT_PTR pTag);


		virtual bool IsVisible() const;
		virtual void SetVisible(bool bVisible = true);
		virtual void SetInternVisible(bool bVisible = true);
		virtual bool IsEnabled() const;
		virtual void SetEnabled(bool bEnable = true);
		virtual bool IsMouseEnabled() const;
		virtual void SetMouseEnabled(bool bEnable = true);
		virtual bool IsKeyboardEnabled() const;
		virtual void SetKeyboardEnabled(bool bEnable = true);
		virtual bool IsFocused() const;
		virtual void SetFocus();
		virtual bool IsFloat() const;
		virtual void SetFloat(bool bFloat = true);

		virtual CControlUI* FindControl(FINDCONTROLPROC Proc, LPVOID pData, UINT uFlags);

		void Invalidate();
		bool IsUpdateNeeded() const;
		void NeedUpdate();
		void NeedParentUpdate();
		DWORD GetAdjustColor(DWORD dwColor);

		virtual void Init();
		virtual void DoInit();

		virtual void Event(TEventUI& event);
		virtual void DoEvent(TEventUI& event);


		void AddCustomAttribute(LPCTSTR pstrName, LPCTSTR pstrAttr);
		LPCTSTR GetCustomAttribute(LPCTSTR pstrName) const;
		bool RemoveCustomAttribute(LPCTSTR pstrName);
		void RemoveAllCustomAttribute();

		virtual void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);
		CControlUI* ApplyAttributeList(LPCTSTR pstrList);

		virtual SIZE EstimateSize(SIZE szAvailable);
		virtual bool Paint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl = NULL);
		virtual bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl);
		virtual void PaintBkColor(HDC hDC);
		virtual void PaintBkImage(HDC hDC);
		virtual void PaintStatusImage(HDC hDC);
		virtual void PaintForeColor(HDC hDC);
		virtual void PaintForeImage(HDC hDC);
		virtual void PaintText(HDC hDC);
		virtual void PaintBorder(HDC hDC);

		virtual void DoPostPaint(HDC hDC, const RECT& rcPaint);


		void SetVirtualWnd(LPCTSTR pstrValue);
		CDuiString GetVirtualWnd() const;

	public:
		CEventSource OnInit;
		CEventSource OnDestroy;
		CEventSource OnSize;
		CEventSource OnEvent;
		CEventSource OnNotify;

	protected:
		CPaintManagerUI* m_pManager;
		CControlUI* m_pParent;
		CDuiString m_sVirtualWnd;
		CDuiString m_sName;
		bool m_bUpdateNeeded;
		bool m_bMenuUsed;
		RECT m_rcItem;
		RECT m_rcPadding;
		SIZE m_cXY;
		SIZE m_cxyFixed;
		SIZE m_cxyMin;
		SIZE m_cxyMax;
		bool m_bVisible;
		bool m_bInternVisible;
		bool m_bEnabled;
		bool m_bMouseEnabled;
		bool m_bKeyboardEnabled;
		bool m_bFocused;
		bool m_bFloat;
		TPercentInfo m_piFloatPercent;
		UINT m_uFloatAlign;
		bool m_bSetPos;

		bool m_bRichEvent;
		bool m_bDragEnabled;
		bool m_bDropEnabled;

		bool m_bResourceText;
		CDuiString m_sText;
		CDuiString m_sToolTip;
		TCHAR m_chShortcut;
		CDuiString m_sUserData;
		UINT_PTR m_pTag;

		CDuiString m_sGradient;
		DWORD m_dwBackColor;
		DWORD m_dwBackColor2;
		DWORD m_dwBackColor3;
		DWORD m_dwForeColor;
		CDuiString m_sBkImage;
		CDuiString m_sForeImage;
		DWORD m_dwBorderColor;
		DWORD m_dwFocusBorderColor;
		bool m_bColorHSL;
		int m_nBorderSize;
		int m_nBorderStyle;
		int m_nTooltipWidth;
		WORD m_wCursor;
		SIZE m_cxyBorderRound;
		RECT m_rcPaint;
		RECT m_rcBorderSize;
	    HINSTANCE m_instance;

		CStdStringPtrMap m_mCustomAttrHash;
	};

}

#endif

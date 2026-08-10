#ifndef __UIMENU_H__
#define __UIMENU_H__

#pragma once

#include "../Utils/observer_impl_base.h"

namespace DuiLib {

struct ContextMenuParam
{


	WPARAM wParam;
	HWND hWnd;
};

struct MenuItemInfo
{
	TCHAR szName[256];
	bool bChecked;
};
struct MenuCmd
{
	TCHAR szName[256];
	TCHAR szUserData[1024];
	TCHAR szText[1024];
	BOOL bChecked;
};

enum MenuAlignment
{
	eMenuAlignment_Left = 1 << 1,
	eMenuAlignment_Top = 1 << 2,
	eMenuAlignment_Right = 1 << 3,
	eMenuAlignment_Bottom = 1 << 4,
};


enum MenuItemDefaultInfo
{
	ITEM_DEFAULT_HEIGHT = 30,
	ITEM_DEFAULT_WIDTH = 150,

	ITEM_DEFAULT_ICON_WIDTH = 26,
	ITEM_DEFAULT_ICON_SIZE = 16,

	ITEM_DEFAULT_EXPLAND_ICON_WIDTH = 20,
	ITEM_DEFAULT_EXPLAND_ICON_SIZE = 9,

	DEFAULT_LINE_LEFT_INSET = ITEM_DEFAULT_ICON_WIDTH + 3,
	DEFAULT_LINE_RIGHT_INSET = 7,
	DEFAULT_LINE_HEIGHT = 6,
	DEFAULT_LINE_COLOR = 0xFFBCBFC4

};


class MenuMenuReceiverImplBase;
class MenuMenuObserverImplBase
{
public:
	virtual void AddReceiver(MenuMenuReceiverImplBase* receiver) = 0;
	virtual void RemoveReceiver(MenuMenuReceiverImplBase* receiver) = 0;
	virtual BOOL RBroadcast(ContextMenuParam param) = 0;
};

class MenuMenuReceiverImplBase
{
public:
	virtual void AddObserver(MenuMenuObserverImplBase* observer) = 0;
	virtual void RemoveObserver() = 0;
	virtual BOOL Receive(ContextMenuParam param) = 0;
};


class MenuReceiverImpl;
class UILIB_API MenuObserverImpl : public MenuMenuObserverImplBase
{
	friend class Iterator;
public:
	MenuObserverImpl():
	m_pMainWndPaintManager(NULL),
	m_pMenuCheckInfo(NULL)
	{
		pReceivers_ = new ReceiversVector;
	}

	~MenuObserverImpl()
	{
		if (pReceivers_ != NULL)
		{
			delete pReceivers_;
			pReceivers_ = NULL;
		}

	}

	virtual void AddReceiver(MenuMenuReceiverImplBase* receiver)
	{
		if (receiver == NULL)
			return;

		pReceivers_->push_back(receiver);
		receiver->AddObserver(this);
	}

	virtual void RemoveReceiver(MenuMenuReceiverImplBase* receiver)
	{
		if (receiver == NULL)
			return;

		ReceiversVector::iterator it = pReceivers_->begin();
		for (; it != pReceivers_->end(); ++it)
		{
			if (*it == receiver)
			{
				pReceivers_->erase(it);
				break;
			}
		}
	}

	virtual BOOL RBroadcast(ContextMenuParam param)
	{
		ReceiversVector::reverse_iterator it = pReceivers_->rbegin();
		for (; it != pReceivers_->rend(); ++it)
		{
			(*it)->Receive(param);
		}

		return BOOL();
	}


	class Iterator
	{
		MenuObserverImpl & _tbl;
		DWORD index;
		MenuMenuReceiverImplBase* ptr;
	public:
		Iterator( MenuObserverImpl & table )
			: _tbl( table ), index(0), ptr(NULL)
		{}

		Iterator( const Iterator & v )
			: _tbl( v._tbl ), index(v.index), ptr(v.ptr)
		{}

		MenuMenuReceiverImplBase* next()
		{
			if ( index >= _tbl.pReceivers_->size() )
				return NULL;

			for ( ; index < _tbl.pReceivers_->size(); )
			{
				ptr = (*(_tbl.pReceivers_))[ index++ ];
				if ( ptr )
					return ptr;
			}
			return NULL;
		}
	};

	virtual void SetManger(CPaintManagerUI* pManager)
	{
		if (pManager != NULL)
			m_pMainWndPaintManager = pManager;
	}

	virtual CPaintManagerUI* GetManager() const
	{
		return m_pMainWndPaintManager;
	}

	virtual void SetMenuCheckInfo(CStdStringPtrMap* pInfo)
	{
		if (pInfo != NULL)
			m_pMenuCheckInfo = pInfo;
		else
			m_pMenuCheckInfo = NULL;
	}

	virtual CStdStringPtrMap* GetMenuCheckInfo() const
	{
		return m_pMenuCheckInfo;
	}

protected:
	typedef std::vector<MenuMenuReceiverImplBase*> ReceiversVector;
	ReceiversVector *pReceivers_;
	CPaintManagerUI* m_pMainWndPaintManager;
	CStdStringPtrMap* m_pMenuCheckInfo;
};


class UILIB_API MenuReceiverImpl : public MenuMenuReceiverImplBase
{
public:
	MenuReceiverImpl()
	{
		pObservers_ = new ObserversVector;
	}

	~MenuReceiverImpl()
	{
		if (pObservers_ != NULL)
		{
			delete pObservers_;
			pObservers_ = NULL;
		}
	}

	virtual void AddObserver(MenuMenuObserverImplBase* observer)
	{
		pObservers_->push_back(observer);
	}

	virtual void RemoveObserver()
	{
		if (!pObservers_) return;

		ObserversVector::iterator it = pObservers_->begin();
		for (; it != pObservers_->end(); ++it)
		{
			(*it)->RemoveReceiver(this);
		}
	}

	virtual BOOL Receive(ContextMenuParam param)
	{
		return BOOL();
	}

protected:
	typedef std::vector<MenuMenuObserverImplBase*> ObserversVector;
	ObserversVector* pObservers_;
};



class CListUI;
class CMenuWnd;
class UILIB_API CMenuUI : public CListUI
{
	DECLARE_DUICONTROL(CMenuUI)
public:
	CMenuUI();
	virtual ~CMenuUI();
	CMenuWnd*	m_pWindow;
    LPCTSTR GetClass() const;
    LPVOID GetInterface(LPCTSTR pstrName);
	UINT GetListType();

	virtual void DoEvent(TEventUI& event);

    virtual bool Add(CControlUI* pControl);
    virtual bool AddAt(CControlUI* pControl, int iIndex);

    virtual int GetItemIndex(CControlUI* pControl) const;
    virtual bool SetItemIndex(CControlUI* pControl, int iIndex);
    virtual bool Remove(CControlUI* pControl);

	SIZE EstimateSize(SIZE szAvailable);

	void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) ;
};



class CMenuElementUI;
class UILIB_API CMenuWnd : public CWindowWnd, public MenuReceiverImpl, public INotifyUI, public IDialogBuilderCallback
{
public:
	static MenuObserverImpl& GetGlobalContextMenuObserver()
	{
		static MenuObserverImpl s_context_menu_observer;
		return s_context_menu_observer;
	}
	static CMenuWnd* CreateMenu(CMenuElementUI* pOwner, STRINGorID xml, POINT point,
		CPaintManagerUI* pMainPaintManager, CStdStringPtrMap* pMenuCheckInfo = NULL,
		DWORD dwAlignment = eMenuAlignment_Left | eMenuAlignment_Top);
	static void DestroyMenu();
	static MenuItemInfo* SetMenuItemInfo(LPCTSTR pstrName, bool bChecked);

public:
	CMenuWnd();
	~CMenuWnd();
	void Close(UINT nRet = IDOK);
	bool isClosing;




    void Init(CMenuElementUI* pOwner, STRINGorID xml, POINT point,
		CPaintManagerUI* pMainPaintManager, CStdStringPtrMap* pMenuCheckInfo = NULL,
		DWORD dwAlignment = eMenuAlignment_Left | eMenuAlignment_Top);
    LPCTSTR GetWindowClassName() const;
    void OnFinalMessage(HWND hWnd);
	void Notify(TNotifyUI& msg);
	CControlUI* CreateControl(LPCTSTR pstrClassName);

	LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnKillFocus(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

	BOOL Receive(ContextMenuParam param);


	CMenuUI* GetMenuUI();


	void ResizeMenu();


	void ResizeSubMenu();
	void setDPI(int DPI);

public:
	bool m_bCaptured;
	POINT			m_BasedPoint;
	STRINGorID		m_xml;
    CPaintManagerUI m_pm;
    CMenuElementUI* m_pOwner;
    CMenuUI*	m_pLayout;
	DWORD		m_dwAlignment;
};

class CListContainerElementUI;
class UILIB_API CMenuElementUI : public CListContainerElementUI
{
	DECLARE_DUICONTROL(CMenuElementUI)
	friend CMenuWnd;
public:
    CMenuElementUI();
	~CMenuElementUI();

    LPCTSTR GetClass() const;
    LPVOID GetInterface(LPCTSTR pstrName);
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl);
	void DrawItemText(HDC hDC, const RECT& rcItem);
	SIZE EstimateSize(SIZE szAvailable);

	void DoEvent(TEventUI& event);

	CMenuWnd* GetMenuWnd();
	void CreateMenuWnd();

	void SetLineType();
	void SetLineColor(DWORD color);
	DWORD GetLineColor() const;
	void SetLinePadding(RECT rcInset);
	RECT GetLinePadding() const;
	void SetIcon(LPCTSTR strIcon);
	void SetIconSize(LONG cx, LONG cy);
	SIZE GetIconSize();
	void DrawItemIcon(HDC hDC, const RECT& rcItem);
	void SetChecked(bool bCheck = true);
	bool GetChecked() const;
	void SetCheckItem(bool bCheckItem = false);
	bool GetCheckItem() const;

	void SetShowExplandIcon(bool bShow);
	void DrawItemExpland(HDC hDC, const RECT& rcItem);

	void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);

	MenuItemInfo* GetItemInfo(LPCTSTR pstrName);
	MenuItemInfo* SetItemInfo(LPCTSTR pstrName, bool bChecked);
protected:
	CMenuWnd*	m_pWindow;

	bool		m_bDrawLine;
	DWORD		m_dwLineColor;
	RECT		m_rcLinePadding;

	SIZE		m_szIconSize;
	CDuiString	m_strIcon;
	bool		m_bCheckItem;

	bool		m_bShowExplandIcon;
};

}

#endif

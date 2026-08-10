#ifndef __UISHADOW_H__
#define __UISHADOW_H__

#pragma once
#include <map>

namespace DuiLib
{

class UILIB_API CShadowUI
{
public:
	friend class CPaintManagerUI;

	CShadowUI(void);
	virtual ~CShadowUI(void);

public:

	void ShowShadow(bool bShow);
	bool IsShowShadow() const;

	void DisableShadow(bool bDisable);
	bool IsDisableShadow() const;


	bool SetSize(int NewSize = 0);
	bool SetSharpness(unsigned int NewSharpness = 5);
	bool SetDarkness(unsigned int NewDarkness = 200);
	bool SetPosition(int NewXOffset = 5, int NewYOffset = 5);
	bool SetColor(COLORREF NewColor = 0);


	bool SetImage(LPCTSTR szImage);
	bool SetShadowCorner(RECT rcCorner);


	bool CopyShadow(CShadowUI* pShadow);


	void Create(CPaintManagerUI* pPaintManager);
protected:


	static bool Initialize(HINSTANCE hInstance);


	static std::map<HWND, CShadowUI *>& GetShadowMap();


	static LRESULT CALLBACK ParentProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);


	void Update(HWND hParent);


	void MakeShadow(UINT32 *pShadBits, HWND hParent, RECT *rcParent);


	inline DWORD PreMultiply(COLORREF cl, unsigned char nAlpha)
	{
		return (GetRValue(cl) * (DWORD)nAlpha / 255) |
			(GetGValue(cl) * (DWORD)nAlpha / 255) << 8 |
			(GetBValue(cl) * (DWORD)nAlpha / 255) << 16 ;
	}

protected:
	enum ShadowStatus
	{
		SS_ENABLED = 1,
		SS_VISABLE = 1 << 1,
		SS_PARENTVISIBLE = 1<< 2
	};


	static bool s_bHasInit;

	CPaintManagerUI	*m_pManager;
	HWND			 m_hWnd;
	LONG_PTR		 m_OriParentProc;
	BYTE			 m_Status;
	bool			 m_bIsImageMode;
	bool			 m_bIsShowShadow;
	bool			m_bIsDisableShadow;

	unsigned char m_nDarkness;
	unsigned char m_nSharpness;
	signed char m_nSize;



	signed char m_nxOffset;
	signed char m_nyOffset;


	LPARAM m_WndSize;


	bool m_bUpdate;

	COLORREF m_Color;


	CDuiString	m_sShadowImage;
	RECT		m_rcShadowCorner;
};

}

#endif

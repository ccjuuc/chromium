#include "UIlib.h"
#include "UIShadow.h"
#include "math.h"
#include "crtdbg.h"

namespace DuiLib
{

const TCHAR *strWndClassName = _T("PerryShadowWnd");
bool CShadowUI::s_bHasInit = FALSE;

CShadowUI::CShadowUI(void)
: m_hWnd((HWND)NULL)
, m_OriParentProc(NULL)
, m_Status(0)
, m_nDarkness(150)
, m_nSharpness(5)
, m_nSize(0)
, m_nxOffset(0)
, m_nyOffset(0)
, m_Color(RGB(0, 0, 0))
, m_WndSize(0)
, m_bUpdate(false)
, m_bIsImageMode(false)
, m_bIsShowShadow(false)
, m_bIsDisableShadow(false)
{
	::ZeroMemory(&m_rcShadowCorner, sizeof(RECT));
}

CShadowUI::~CShadowUI(void)
{
}

bool CShadowUI::Initialize(HINSTANCE hInstance)
{
	if (s_bHasInit)
		return false;


	WNDCLASSEX wcex;

	memset(&wcex, 0, sizeof(wcex));

	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style			= CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc	= DefWindowProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= NULL;
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName	= NULL;
	wcex.lpszClassName	= strWndClassName;
	wcex.hIconSm		= NULL;

	RegisterClassEx(&wcex);

	s_bHasInit = true;
	return true;
}

void CShadowUI::Create(CPaintManagerUI* pPaintManager)
{
	if(!m_bIsShowShadow)
		return;


	_ASSERT(CPaintManagerUI::GetInstance() != INVALID_HANDLE_VALUE);
	_ASSERT(pPaintManager != NULL);
	m_pManager = pPaintManager;
	HWND hParentWnd = m_pManager->GetPaintWindow();

	_ASSERT(GetShadowMap().find(hParentWnd) == GetShadowMap().end());
	GetShadowMap()[hParentWnd] = this;


	LONG lParentStyle = GetWindowLongPtr(hParentWnd, GWL_STYLE);


	LONG styleValue = lParentStyle & WS_CAPTION;
	m_hWnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TRANSPARENT, strWndClassName, NULL,
		styleValue | WS_POPUPWINDOW,
		CW_USEDEFAULT, 0, 0, 0, hParentWnd, NULL, CPaintManagerUI::GetInstance(), NULL);

	if(!(WS_VISIBLE & lParentStyle))
		m_Status = SS_ENABLED;
	else if((WS_MAXIMIZE | WS_MINIMIZE) & lParentStyle)
		m_Status = SS_ENABLED | SS_PARENTVISIBLE;
	else
	{
		m_Status = SS_ENABLED | SS_VISABLE | SS_PARENTVISIBLE;
		::ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
		Update(hParentWnd);
	}


	m_OriParentProc = GetWindowLongPtr(hParentWnd, GWLP_WNDPROC);

#pragma warning(disable: 4311)
	SetWindowLongPtr(hParentWnd, GWLP_WNDPROC, (LONG_PTR)ParentProc);
#pragma warning(default: 4311)

}

std::map<HWND, CShadowUI *>& CShadowUI::GetShadowMap()
{
	static std::map<HWND, CShadowUI *> s_Shadowmap;
	return s_Shadowmap;
}

LRESULT CALLBACK CShadowUI::ParentProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	_ASSERT(GetShadowMap().find(hwnd) != GetShadowMap().end());

	CShadowUI *pThis = GetShadowMap()[hwnd];
	if (pThis->IsDisableShadow()) {
		return CallWindowProc((WNDPROC)pThis->m_OriParentProc, hwnd, uMsg, wParam, lParam);
	}

	switch(uMsg)
	{
	case WM_ACTIVATEAPP:
		{
			if(!IsWindowEnabled(hwnd)) break;
			::SetWindowPos(pThis->m_hWnd, hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
			break;
		}
	case WM_NCACTIVATE:
		{
			if(!IsWindowEnabled(hwnd)) break;
			::SetWindowPos(pThis->m_hWnd, hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
			break;
		}
	case WM_WINDOWPOSCHANGED:
		{
			RECT WndRect;
			LPWINDOWPOS pWndPos;
			pWndPos = (LPWINDOWPOS)lParam;
			GetWindowRect(hwnd, &WndRect);
			if (pThis->m_bIsImageMode) {
				SetWindowPos(pThis->m_hWnd, hwnd, WndRect.left - pThis->m_nSize, WndRect.top - pThis->m_nSize, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			}
			else {
				SetWindowPos(pThis->m_hWnd, hwnd, WndRect.left + pThis->m_nxOffset - pThis->m_nSize, WndRect.top + pThis->m_nyOffset - pThis->m_nSize, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			}

			if (pWndPos->flags & SWP_SHOWWINDOW) {
				if (pThis->m_Status & SS_ENABLED && !(pThis->m_Status & SS_PARENTVISIBLE))
				{
					pThis->m_bUpdate = true;
					::ShowWindow(pThis->m_hWnd, SW_SHOWNOACTIVATE);
					pThis->m_Status |= SS_VISABLE | SS_PARENTVISIBLE;
				}
			}
			else if (pWndPos->flags & SWP_HIDEWINDOW) {
				if (pThis->m_Status & SS_ENABLED)
				{
					::ShowWindow(pThis->m_hWnd, SW_HIDE);
					pThis->m_Status &= ~(SS_VISABLE | SS_PARENTVISIBLE);
				}
			}
			break;
		}

	case WM_MOVE:
		if(pThis->m_Status & SS_VISABLE) {
			RECT WndRect;
			GetWindowRect(hwnd, &WndRect);
			if (pThis->m_bIsImageMode) {
				SetWindowPos(pThis->m_hWnd, hwnd, WndRect.left - pThis->m_nSize, WndRect.top - pThis->m_nSize, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			}
			else {
				SetWindowPos(pThis->m_hWnd, hwnd, WndRect.left + pThis->m_nxOffset - pThis->m_nSize, WndRect.top + pThis->m_nyOffset - pThis->m_nSize, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			}
		}
		break;

	case WM_SIZE:
		if(pThis->m_Status & SS_ENABLED) {
			if(SIZE_MAXIMIZED == wParam || SIZE_MINIMIZED == wParam) {
				::ShowWindow(pThis->m_hWnd, SW_HIDE);
				pThis->m_Status &= ~SS_VISABLE;
			}

			else if(pThis->m_Status & SS_PARENTVISIBLE) {



				if(LOWORD(lParam) > LOWORD(pThis->m_WndSize) || HIWORD(lParam) > HIWORD(pThis->m_WndSize))
					pThis->m_bUpdate = true;
				else
					pThis->Update(hwnd);

				if(!(pThis->m_Status & SS_VISABLE)) {
					::ShowWindow(pThis->m_hWnd, SW_SHOWNOACTIVATE);
					pThis->m_Status |= SS_VISABLE;
				}
			}
			pThis->m_WndSize = lParam;
		}
		break;

	case WM_PAINT:
		{
			if(pThis->m_bUpdate) {
				pThis->Update(hwnd);
				pThis->m_bUpdate = false;
			}
			break;
		}



	case WM_EXITSIZEMOVE:
		if(pThis->m_Status & SS_VISABLE) {
			pThis->Update(hwnd);
		}
		break;

	case WM_SHOWWINDOW:
		if(pThis->m_Status & SS_ENABLED) {

			if(!wParam) {
				::ShowWindow(pThis->m_hWnd, SW_HIDE);
				pThis->m_Status &= ~(SS_VISABLE | SS_PARENTVISIBLE);
			}
			else if(!(pThis->m_Status & SS_PARENTVISIBLE)) {

				pThis->m_bUpdate = true;
				::ShowWindow(pThis->m_hWnd, SW_SHOWNOACTIVATE);
				pThis->m_Status |= SS_VISABLE | SS_PARENTVISIBLE;
			}
		}
		break;

	case WM_DESTROY:
		DestroyWindow(pThis->m_hWnd);
		break;
	case WM_NCDESTROY:
		GetShadowMap().erase(hwnd);
		break;
	}

	return CallWindowProc((WNDPROC)pThis->m_OriParentProc, hwnd, uMsg, wParam, lParam);
}


void GetLastErrorMessage()
{
	LPVOID lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL
	);





	LocalFree(lpMsgBuf);
}

void CShadowUI::Update(HWND hParent)
{
	if(!m_bIsShowShadow || !(m_Status & SS_VISABLE)) return;
	RECT WndRect;
	GetWindowRect(hParent, &WndRect);
	int nShadWndWid;
	int nShadWndHei;
	if (m_bIsImageMode) {
		if(m_sShadowImage.IsEmpty()) return;
		nShadWndWid = WndRect.right - WndRect.left + m_nSize * 2;
		nShadWndHei = WndRect.bottom - WndRect.top + m_nSize * 2;
	}
	else {
		if (m_nSize == 0) return;
		nShadWndWid = WndRect.right - WndRect.left + m_nSize * 2;
		nShadWndHei = WndRect.bottom - WndRect.top + m_nSize * 2;
	}


	BITMAPINFO bmi;
	ZeroMemory(&bmi, sizeof(BITMAPINFO));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = nShadWndWid;
	bmi.bmiHeader.biHeight = nShadWndHei;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	bmi.bmiHeader.biSizeImage = nShadWndWid * nShadWndHei * 4;
	BYTE *pvBits;
	HBITMAP hbitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)&pvBits, NULL, 0);
	if (hbitmap == NULL) {
		GetLastErrorMessage();
	}

	HDC hMemDC = CreateCompatibleDC(NULL);
	if (hMemDC == NULL) {
		GetLastErrorMessage();
	}
	HBITMAP hOriBmp = (HBITMAP)SelectObject(hMemDC, hbitmap);
	if (GetLastError()!=0) {
		GetLastErrorMessage();
	}
	if (m_bIsImageMode) {
		RECT rcPaint = {0, 0, nShadWndWid, nShadWndHei};
		const TDrawInfo* pDrawInfo = m_pManager->GetDrawInfo(m_sShadowImage, _T(""));
		if(pDrawInfo != NULL) {
			const TImageInfo* data = m_pManager->GetImageEx(pDrawInfo->sImageName, pDrawInfo->sResType, pDrawInfo->dwMask, pDrawInfo->bHSL, pDrawInfo->bGdiplus);
			if( !data ) return;
			RECT rcBmpPart = {0};
			rcBmpPart.right = data->nX;
			rcBmpPart.bottom = data->nY;
			RECT corner = m_rcShadowCorner;
			CRenderEngine::DrawImage(hMemDC, data->hBitmap, rcPaint, rcPaint, rcBmpPart, corner, data->bAlpha, 0xFF, true, false, false);
		}
	}
	else {
		ZeroMemory(pvBits, bmi.bmiHeader.biSizeImage);
		MakeShadow((UINT32 *)pvBits, hParent, &WndRect);
	}

	POINT ptDst;
	if (m_bIsImageMode) {
		ptDst.x = WndRect.left - m_nSize;
		ptDst.y = WndRect.top - m_nSize;
	}
	else {
		ptDst.x = WndRect.left + m_nxOffset - m_nSize;
		ptDst.y = WndRect.top + m_nyOffset - m_nSize;
	}

	POINT ptSrc = {0, 0};
	SIZE WndSize = {nShadWndWid, nShadWndHei};
	BLENDFUNCTION blendPixelFunction= { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
	MoveWindow(m_hWnd, ptDst.x, ptDst.y, nShadWndWid, nShadWndHei, FALSE);
	BOOL bRet= ::UpdateLayeredWindow(m_hWnd, NULL, &ptDst, &WndSize, hMemDC, &ptSrc, 0, &blendPixelFunction, ULW_ALPHA);
	_ASSERT(bRet);

	SelectObject(hMemDC, hOriBmp);
	DeleteObject(hbitmap);
	DeleteDC(hMemDC);
}

void CShadowUI::MakeShadow(UINT32 *pShadBits, HWND hParent, RECT *rcParent)
{







	HRGN hParentRgn = CreateRectRgn(0, 0, abs(rcParent->right - rcParent->left), abs(rcParent->bottom - rcParent->top));
	GetWindowRgn(hParent, hParentRgn);


	SIZE szParent = {rcParent->right - rcParent->left, rcParent->bottom - rcParent->top};
	SIZE szShadow = {szParent.cx + 2 * m_nSize, szParent.cy + 2 * m_nSize};

	int nAnchors = max(szParent.cy, szShadow.cy);
	int (*ptAnchors)[2] = new int[nAnchors + 2][2];
	int (*ptAnchorsOri)[2] = new int[szParent.cy][2];
	ptAnchors[0][0] = szParent.cx;
	ptAnchors[0][1] = 0;
	ptAnchors[nAnchors + 1][0] = szParent.cx;
	ptAnchors[nAnchors + 1][1] = 0;
	if(m_nSize > 0) {

		for(int i = 0; i < m_nSize; i++) {
			ptAnchors[i + 1][0] = szParent.cx;
			ptAnchors[i + 1][1] = 0;
			ptAnchors[szShadow.cy - i][0] = szParent.cx;
			ptAnchors[szShadow.cy - i][1] = 0;
		}
		ptAnchors += m_nSize;
	}
	for(int i = 0; i < szParent.cy; i++) {

		int j;
		for(j = 0; j < szParent.cx; j++) {
			if(PtInRegion(hParentRgn, j, i)) {
				ptAnchors[i + 1][0] = j + m_nSize;
				ptAnchorsOri[i][0] = j;
				break;
			}
		}

		if(j >= szParent.cx) {
			ptAnchors[i + 1][0] = szParent.cx;
			ptAnchorsOri[i][1] = 0;
			ptAnchors[i + 1][0] = szParent.cx;
			ptAnchorsOri[i][1] = 0;
		}
		else {

			for(j = szParent.cx - 1; j >= ptAnchors[i + 1][0]; j--) {
				if(PtInRegion(hParentRgn, j, i)) {
					ptAnchors[i + 1][1] = j + 1 + m_nSize;
					ptAnchorsOri[i][1] = j + 1;
					break;
				}
			}
		}
	}

	if(m_nSize > 0)
		ptAnchors -= m_nSize;
	int (*ptAnchorsTmp)[2] = new int[nAnchors + 2][2];

	ptAnchorsTmp[0][0] = szParent.cx;
	ptAnchorsTmp[0][1] = 0;
	ptAnchorsTmp[nAnchors + 1][0] = szParent.cx;
	ptAnchorsTmp[nAnchors + 1][1] = 0;
	int nEroTimes = 0;

	for(int i = 0; i < m_nSharpness - m_nSize; i++) {
		nEroTimes++;




		for(int j = 1; j < nAnchors + 1; j++) {
			ptAnchorsTmp[j][0] = max(ptAnchors[j - 1][0], max(ptAnchors[j][0], ptAnchors[j + 1][0])) + 1;
			ptAnchorsTmp[j][1] = min(ptAnchors[j - 1][1], min(ptAnchors[j][1], ptAnchors[j + 1][1])) - 1;
		}

		int (*ptAnchorsXange)[2] = ptAnchorsTmp;
		ptAnchorsTmp = ptAnchors;
		ptAnchors = ptAnchorsXange;
	}


	ptAnchors += (m_nSize < 0 ? -m_nSize : 0) + 1;

	int nKernelSize = m_nSize > m_nSharpness ? m_nSize : m_nSharpness;
	int nCenterSize = m_nSize > m_nSharpness ? (m_nSize - m_nSharpness) : 0;
	UINT32 *pKernel = new UINT32[(2 * nKernelSize + 1) * (2 * nKernelSize + 1)];
	UINT32 *pKernelIter = pKernel;
	for(int i = 0; i <= 2 * nKernelSize; i++) {
		for(int j = 0; j <= 2 * nKernelSize; j++) {
			double dLength = sqrt((i - nKernelSize) * (i - nKernelSize) + (j - nKernelSize) * (double)(j - nKernelSize));
			if(dLength < nCenterSize)
				*pKernelIter = m_nDarkness << 24 | PreMultiply(m_Color, m_nDarkness);
			else if(dLength <= nKernelSize) {
				UINT32 nFactor = ((UINT32)((1 - (dLength - nCenterSize) / (m_nSharpness + 1)) * m_nDarkness));
				*pKernelIter = nFactor << 24 | PreMultiply(m_Color, nFactor);
			}
			else
				*pKernelIter = 0;

			pKernelIter ++;
		}

	}

	for(int i = nKernelSize; i < szShadow.cy - nKernelSize; i++)
	{
		int j;
		if(ptAnchors[i][0] < ptAnchors[i][1])
		{

			for(j = ptAnchors[i][0]; j < min(max(ptAnchors[i - 1][0], ptAnchors[i + 1][0]) + 1, ptAnchors[i][1]); j++)
			{
				for(int k = 0; k <= 2 * nKernelSize; k++)
				{
					UINT32 *pPixel = pShadBits +
						(szShadow.cy - i - 1 + nKernelSize - k) * szShadow.cx + j - nKernelSize;
					UINT32 *pKernelPixel = pKernel + k * (2 * nKernelSize + 1);
					for(int l = 0; l <= 2 * nKernelSize; l++)
					{
						if(*pPixel < *pKernelPixel)
							*pPixel = *pKernelPixel;
						pPixel++;
						pKernelPixel++;
					}
				}
			}


			for(j = max(j, min(ptAnchors[i - 1][1], ptAnchors[i + 1][1]) - 1); j < ptAnchors[i][1]; j++)
			{
				for(int k = 0; k <= 2 * nKernelSize; k++)
				{
					UINT32 *pPixel = pShadBits +
						(szShadow.cy - i - 1 + nKernelSize - k) * szShadow.cx + j - nKernelSize;
					UINT32 *pKernelPixel = pKernel + k * (2 * nKernelSize + 1);
					for(int l = 0; l <= 2 * nKernelSize; l++)
					{
						if(*pPixel < *pKernelPixel)
							*pPixel = *pKernelPixel;
						pPixel++;
						pKernelPixel++;
					}
				}
			}

		}
	}


	UINT32 clCenter = m_nDarkness << 24 | PreMultiply(m_Color, m_nDarkness);
	for(int i = min(nKernelSize, max(m_nSize - m_nyOffset, 0)); i < max(szShadow.cy - nKernelSize, min(szParent.cy + m_nSize - m_nyOffset, szParent.cy + 2 * m_nSize)); i++) {
		UINT32 *pLine = pShadBits + (szShadow.cy - i - 1) * szShadow.cx;

		if(i - m_nSize + m_nyOffset < 0 || i - m_nSize + m_nyOffset >= szParent.cy)	{
			for(int j = ptAnchors[i][0]; j < ptAnchors[i][1]; j++) {
				*(pLine + j) = clCenter;
			}
		}
		else {
			for(int j = ptAnchors[i][0]; j < min(ptAnchorsOri[i - m_nSize + m_nyOffset][0] + m_nSize - m_nxOffset, ptAnchors[i][1]); j++)
				*(pLine + j) = clCenter;
			for(int j = max(ptAnchorsOri[i - m_nSize + m_nyOffset][0] + m_nSize - m_nxOffset, 0); j < min(ptAnchorsOri[i - m_nSize + m_nyOffset][1] + m_nSize - m_nxOffset, szShadow.cx); j++)
				*(pLine + j) = 0;
			for(int j = max(ptAnchorsOri[i - m_nSize + m_nyOffset][1] + m_nSize - m_nxOffset, ptAnchors[i][0]); j < ptAnchors[i][1]; j++)
				*(pLine + j) = clCenter;
		}
	}


	delete[] (ptAnchors - (m_nSize < 0 ? -m_nSize : 0) - 1);
	delete[] ptAnchorsTmp;
	delete[] ptAnchorsOri;
	delete[] pKernel;
	DeleteObject(hParentRgn);
}

void CShadowUI::ShowShadow(bool bShow)
{
	m_bIsShowShadow = bShow;
}

bool CShadowUI::IsShowShadow() const
{
	return m_bIsShowShadow;
}


void CShadowUI::DisableShadow(bool bDisable)
{
	m_bIsDisableShadow = bDisable;
	if (m_hWnd != NULL) {
		if (m_bIsDisableShadow) {
			::ShowWindow(m_hWnd, SW_HIDE);
		}
		else {

			LONG lParentStyle = GetWindowLongPtr(GetParent(m_hWnd), GWL_STYLE);

			if (!(WS_VISIBLE & lParentStyle))
				m_Status = SS_ENABLED;
			else if ((WS_MAXIMIZE | WS_MINIMIZE) & lParentStyle)
				m_Status = SS_ENABLED | SS_PARENTVISIBLE;
			else {
				m_Status = SS_ENABLED | SS_VISABLE | SS_PARENTVISIBLE;
			}

			if ((WS_VISIBLE & lParentStyle) && !((WS_MAXIMIZE | WS_MINIMIZE) & lParentStyle)) {
				::ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
				Update(GetParent(m_hWnd));
			}
		}
	}
}


bool CShadowUI::IsDisableShadow() const
{
	return m_bIsDisableShadow;
}

bool CShadowUI::SetSize(int NewSize)
{
	if(NewSize > 35 || NewSize < -35)
		return false;

	m_nSize = (signed char)NewSize;
	if(m_hWnd != NULL && (SS_VISABLE & m_Status))
		Update(GetParent(m_hWnd));
	return true;
}

bool CShadowUI::SetSharpness(unsigned int NewSharpness)
{
	if(NewSharpness > 35)
		return false;

	m_nSharpness = (unsigned char)NewSharpness;
	if(m_hWnd != NULL && (SS_VISABLE & m_Status))
		Update(GetParent(m_hWnd));
	return true;
}

bool CShadowUI::SetDarkness(unsigned int NewDarkness)
{
	if(NewDarkness > 255)
		return false;

	m_nDarkness = (unsigned char)NewDarkness;
	if(m_hWnd != NULL && (SS_VISABLE & m_Status))
		Update(GetParent(m_hWnd));
	return true;
}

bool CShadowUI::SetPosition(int NewXOffset, int NewYOffset)
{
	if(NewXOffset > 35 || NewXOffset < -35 ||
		NewYOffset > 35 || NewYOffset < -35)
		return false;

	m_nxOffset = (signed char)NewXOffset;
	m_nyOffset = (signed char)NewYOffset;
	if(m_hWnd != NULL && (SS_VISABLE & m_Status))
		Update(GetParent(m_hWnd));

	return true;
}

bool CShadowUI::SetColor(COLORREF NewColor)
{
	m_Color = NewColor;
	if(m_hWnd != NULL && (SS_VISABLE & m_Status))
		Update(GetParent(m_hWnd));

	return true;
}

bool CShadowUI::SetImage(LPCTSTR szImage)
{
	if (szImage == NULL)
		return false;

	m_bIsImageMode = true;
	m_sShadowImage = szImage;
	if(m_hWnd != NULL && (SS_VISABLE & m_Status))
		Update(GetParent(m_hWnd));

	return true;
}

bool CShadowUI::SetShadowCorner(RECT rcCorner)
{
	if (rcCorner.left < 0 || rcCorner.top < 0 || rcCorner.right < 0 || rcCorner.bottom < 0) return false;

	m_rcShadowCorner = rcCorner;
	if(m_hWnd != NULL && (SS_VISABLE & m_Status)) {
		Update(GetParent(m_hWnd));
	}

	return true;
}

bool CShadowUI::CopyShadow(CShadowUI* pShadow)
{
	if (m_bIsImageMode) {
		pShadow->SetImage(m_sShadowImage);
		pShadow->SetShadowCorner(m_rcShadowCorner);
		pShadow->SetSize((int)m_nSize);
	}
	else {
		pShadow->SetSize((int)m_nSize);
		pShadow->SetSharpness((unsigned int)m_nSharpness);
		pShadow->SetDarkness((unsigned int)m_nDarkness);
		pShadow->SetColor(m_Color);
		pShadow->SetPosition((int)m_nxOffset, (int)m_nyOffset);
	}

	pShadow->ShowShadow(m_bIsShowShadow);
	return true;
}
}

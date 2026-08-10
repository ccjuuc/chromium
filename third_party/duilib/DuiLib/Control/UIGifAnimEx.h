#ifndef GifAnimUIEX_h__
#define GifAnimUIEX_h__
#pragma once


#ifdef USE_XIMAGE_EFFECT
namespace DuiLib
{
	class CLabelUI;

	class UILIB_API CGifAnimExUI : public CLabelUI
	{
		DECLARE_DUICONTROL(CGifAnimExUI)
	public:
		CGifAnimExUI(void);
		~CGifAnimExUI(void);
	public:
		virtual LPCTSTR	GetClass() const;
		virtual LPVOID	GetInterface(LPCTSTR pstrName);
		virtual void Init();
		virtual void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);
		virtual void SetVisible(bool bVisible = true);
		virtual void SetInternVisible(bool bVisible = true);
		virtual bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl);
		virtual void DoEvent(TEventUI& event);
	public:
		void StartAnim();
		void StopAnim();
	protected:
		struct Imp;
		Imp* m_pImp;
	};
}
#endif
#endif

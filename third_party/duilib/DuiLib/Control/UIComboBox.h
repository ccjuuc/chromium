#ifndef __UICOMBOBOX_H__
#define __UICOMBOBOX_H__

#pragma once

namespace DuiLib
{



	class UILIB_API CComboBoxUI : public CComboUI
	{
		DECLARE_DUICONTROL(CComboBoxUI)
	public:
		CComboBoxUI();
		LPCTSTR GetClass() const;

		void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);

		void PaintText(HDC hDC);
		void PaintStatusImage(HDC hDC);

	protected:
		CDuiString m_sArrowImage;
		int        m_nArrowWidth;
	};
}

#endif

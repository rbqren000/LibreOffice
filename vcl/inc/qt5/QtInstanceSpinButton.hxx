/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "QtInstanceWidget.hxx"

#include <QtWidgets/QSpinBox>

class QtInstanceSpinButton : public QtInstanceWidget, public virtual weld::SpinButton
{
    QDoubleSpinBox* m_pSpinBox;

public:
    QtInstanceSpinButton(QDoubleSpinBox* pSpinBox);

    virtual void set_text(const OUString& rText) override;
    virtual OUString get_text() const override;
    virtual void set_width_chars(int nChars) override;
    virtual int get_width_chars() const override;
    virtual void set_max_length(int nChars) override;
    virtual void select_region(int nStartPos, int nEndPos) override;
    virtual bool get_selection_bounds(int& rStartPos, int& rEndPos) override;
    virtual void replace_selection(const OUString& rText) override;
    virtual void set_position(int nCursorPos) override;
    virtual int get_position() const override;
    virtual void set_editable(bool bEditable) override;
    virtual bool get_editable() const override;
    virtual void set_message_type(weld::EntryMessageType eType) override;
    virtual void set_placeholder_text(const OUString& rText) override;

    virtual void set_overwrite_mode(bool bOn) override;
    virtual bool get_overwrite_mode() const override;

    virtual void set_font(const vcl::Font& rFont) override;
    virtual void set_font_color(const Color& rColor) override;

    virtual void cut_clipboard() override;
    virtual void copy_clipboard() override;
    virtual void paste_clipboard() override;

    virtual void set_alignment(TxtAlign eXAlign) override;

    virtual void set_value(sal_Int64 nValue) override;
    virtual sal_Int64 get_value() const override;
    virtual void set_range(sal_Int64 nMin, sal_Int64 nMax) override;
    virtual void get_range(sal_Int64& rMin, sal_Int64& rMax) const override;

    virtual void set_increments(sal_Int64 nStep, sal_Int64 nPage) override;
    virtual void get_increments(sal_Int64& rStep, sal_Int64& rPage) const override;
    virtual void set_digits(unsigned int nDigits) override;
    virtual unsigned int get_digits() const override;
};

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <vector>

#include <QtCore/QObject>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#include <rtl/ustring.hxx>
#include <unotools/resmgr.hxx>
#include <vcl/builder.hxx>

class QtBuilder : public WidgetBuilder<QObject, QObject*>
{
private:
    QObject* get_by_name(std::u16string_view sID);
    struct WinAndId
    {
        OUString m_sID;
        QObject* m_pWindow;
        WinAndId(OUString sId, QObject* pWindow)
            : m_sID(std::move(sId))
            , m_pWindow(pWindow)
        {
        }
    };

    std::vector<WinAndId> m_aChildren;

public:
    QtBuilder(QObject* pParent, std::u16string_view sUIRoot, const OUString& rUIFile);
    virtual ~QtBuilder();

    template <typename T = QObject> T* get(std::u16string_view sID);

    QObject* makeObject(QObject* pParent, std::u16string_view sName, const OUString& sID,
                        stringmap& rMap);

    virtual void applyAtkProperties(QObject* pObject, const stringmap& rProperties,
                                    bool bToolbarItem) override;
    virtual void applyPackingProperties(QObject* pCurrentChild, QObject* pParent,
                                        const stringmap& rPackingProperties) override;
    virtual void insertComboBoxOrListBoxItems(QObject* pObject, stringmap& rMap,
                                              const std::vector<ComboBoxTextItem>& rItems) override;

    virtual QObject* insertObject(QObject* pParent, const OUString& rClass, const OUString& rID,
                                  stringmap& rProps, stringmap& rPangoAttributes,
                                  stringmap& rAtkProps) override;

    void tweakInsertedChild(QObject* pParent, QObject* pCurrentChild, std::string_view sType,
                            std::string_view sInternalChild) override;

    virtual void setMnemonicWidget(const OUString& rLabelId,
                                   const OUString& rMnemonicWidgetId) override;
    virtual void setPriority(QObject* pObject, int nPriority) override;
    virtual void setContext(QObject* pObject,
                            std::vector<vcl::EnumContext::Context>&& aContext) override;

    virtual void set_response(std::u16string_view sID, short nResponse) override;

private:
    void setProperties(QObject* obj, stringmap& rProps);
    static QWidget* windowForObject(QObject* pObject);
    static QDialogButtonBox* findButtonBox(QDialog* pDialog);

    static void applyGridPackingProperties(QObject* pCurrentChild, QGridLayout& rGrid,
                                           const stringmap& rPackingProperties);
};

template <typename T> inline T* QtBuilder::get(std::u16string_view sID)
{
    QObject* w = get_by_name(sID);
    return static_cast<T*>(w);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

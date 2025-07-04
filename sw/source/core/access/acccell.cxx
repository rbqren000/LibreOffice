/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <sal/log.hxx>
#include <com/sun/star/accessibility/AccessibleRole.hpp>
#include <com/sun/star/accessibility/AccessibleStateType.hpp>
#include <com/sun/star/accessibility/AccessibleEventId.hpp>
#include <cppuhelper/typeprovider.hxx>
#include <vcl/svapp.hxx>
#include <cellfrm.hxx>
#include <tabfrm.hxx>
#include <swtable.hxx>
#include <crsrsh.hxx>
#include <viscrs.hxx>
#include "accfrmobj.hxx"
#include "accfrmobjslist.hxx"
#include <frmfmt.hxx>
#include <cellatr.hxx>
#include <accmap.hxx>
#include "acccell.hxx"

#include <cfloat>
#include <string_view>

#include <editeng/brushitem.hxx>
#include <swatrset.hxx>
#include <frmatr.hxx>
#include "acctable.hxx"

using namespace ::com::sun::star;
using namespace ::com::sun::star::accessibility;
using namespace sw::access;

bool SwAccessibleCell::IsSelected()
{
    bool bRet = false;

    assert(GetMap());
    const SwViewShell& rVSh = GetMap()->GetShell();
    if( auto pCSh = dynamic_cast<const SwCursorShell*>(&rVSh) )
    {
        if( pCSh->IsTableMode() )
        {
            const SwCellFrame *pCFrame =
                static_cast< const SwCellFrame * >( GetFrame() );
            const SwTableBox* pBox = pCFrame->GetTabBox();
            SwSelBoxes const& rBoxes(pCSh->GetTableCursor()->GetSelectedBoxes());
            bRet = rBoxes.find(pBox) != rBoxes.end();
        }
    }

    return bRet;
}

void SwAccessibleCell::GetStates( sal_Int64& rStateSet )
{
    SwAccessibleContext::GetStates( rStateSet );

    // SELECTABLE
    const SwViewShell& rVSh = GetMap()->GetShell();
    if( dynamic_cast<const SwCursorShell*>(&rVSh) !=  nullptr )
        rStateSet |= AccessibleStateType::SELECTABLE;
    //Add resizable state to table cell.
    rStateSet |= AccessibleStateType::RESIZABLE;

    if (IsDisposing()) // tdf#135098
        return;

    // SELECTED
    if( IsSelected() )
    {
        rStateSet |= AccessibleStateType::SELECTED;
        SAL_WARN_IF(!m_bIsSelected, "sw.a11y", "bSelected out of sync");
        ::rtl::Reference < SwAccessibleContext > xThis( this );
        GetMap()->SetCursorContext( xThis );
    }
}

SwAccessibleCell::SwAccessibleCell(std::shared_ptr<SwAccessibleMap> const& pInitMap,
                                    const SwCellFrame *pCellFrame )
    : SwAccessibleCell_BASE(pInitMap, AccessibleRole::TABLE_CELL, pCellFrame)
    , m_aSelectionHelper( *this )
    , m_bIsSelected( false )
{
    OUString sBoxName( pCellFrame->GetTabBox()->GetName() );
    SetName( sBoxName );

    m_bIsSelected = IsSelected();

    rtl::Reference<SwAccessibleContext> xTableReference(
        getAccessibleParentImpl());
    SAL_WARN_IF(
        (!xTableReference.is()
         || xTableReference->getAccessibleRole() != AccessibleRole::TABLE),
        "sw.a11y", "bad accessible context");
    m_pAccTable = static_cast<SwAccessibleTable *>(xTableReference.get());
}

bool SwAccessibleCell::InvalidateMyCursorPos()
{
    bool bNew = IsSelected();
    bool bOld;
    {
        std::scoped_lock aGuard( m_Mutex );
        bOld = m_bIsSelected;
        m_bIsSelected = bNew;
    }
    if( bNew )
    {
        // remember that object as the one that has the caret. This is
        // necessary to notify that object if the cursor leaves it.
        ::rtl::Reference < SwAccessibleContext > xThis( this );
        GetMap()->SetCursorContext( xThis );
    }

    bool bChanged = bOld != bNew;
    if( bChanged )
    {
        FireStateChangedEvent( AccessibleStateType::SELECTED, bNew );
        if (m_pAccTable.is())
        {
            m_pAccTable->AddSelectionCell(this,bNew);
        }
    }
    return bChanged;
}

bool SwAccessibleCell::InvalidateChildrenCursorPos( const SwFrame *pFrame )
{
    bool bChanged = false;

    const SwAccessibleChildSList aVisList( GetVisArea(), *pFrame, *GetMap() );
    SwAccessibleChildSList::const_iterator aIter( aVisList.begin() );
    while( aIter != aVisList.end() )
    {
        const SwAccessibleChild& rLower = *aIter;
        const SwFrame *pLower = rLower.GetSwFrame();
        if( pLower )
        {
            if (rLower.IsAccessible(GetMap()->GetShell().IsPreview()))
            {
                ::rtl::Reference< SwAccessibleContext > xAccImpl(
                    GetMap()->GetContextImpl( pLower, false ) );
                if( xAccImpl.is() )
                {
                    assert(xAccImpl->GetFrame()->IsCellFrame());
                    bChanged = static_cast< SwAccessibleCell *>(
                            xAccImpl.get() )->InvalidateMyCursorPos();
                }
                else
                    bChanged = true; // If the context is not know we
                                         // don't know whether the selection
                                         // changed or not.
            }
            else
            {
                // This is a box with sub rows.
                bChanged |= InvalidateChildrenCursorPos( pLower );
            }
        }
        ++aIter;
    }

    return bChanged;
}

void SwAccessibleCell::InvalidateCursorPos_()
{
    if (IsSelected())
    {
        const SwAccessibleChild aChild( GetChild( *(GetMap()), 0 ) );
        if( aChild.IsValid()  && aChild.GetSwFrame() )
        {
            ::rtl::Reference < SwAccessibleContext > xChildImpl( GetMap()->GetContextImpl( aChild.GetSwFrame())  );
            if (xChildImpl.is())
            {
                xChildImpl->FireAccessibleEvent(AccessibleEventId::STATE_CHANGED, uno::Any(),
                                                uno::Any(AccessibleStateType::FOCUSED));
            }
        }
    }

    const SwFrame *pParent = GetParent( SwAccessibleChild(GetFrame()), IsInPagePreview() );
    assert(pParent->IsTabFrame());
    const SwTabFrame *pTabFrame = static_cast< const SwTabFrame * >( pParent );
    if( pTabFrame->IsFollow() )
        pTabFrame = pTabFrame->FindMaster();

    while( pTabFrame )
    {
        InvalidateChildrenCursorPos( pTabFrame );
        pTabFrame = pTabFrame->GetFollow();
    }
    if (m_pAccTable.is())
    {
        m_pAccTable->FireSelectionEvent();
    }
}

bool SwAccessibleCell::HasCursor()
{
    std::scoped_lock aGuard( m_Mutex );
    return m_bIsSelected;
}

SwAccessibleCell::~SwAccessibleCell()
{
}

OUString SAL_CALL SwAccessibleCell::getAccessibleDescription()
{
    return GetName();
}

OUString SAL_CALL SwAccessibleCell::getImplementationName()
{
    return u"com.sun.star.comp.Writer.SwAccessibleCellView"_ustr;
}

uno::Sequence< OUString > SAL_CALL SwAccessibleCell::getSupportedServiceNames()
{
    return { u"com.sun.star.table.AccessibleCellView"_ustr, sAccessibleServiceName };
}

void SwAccessibleCell::Dispose(bool bRecursive, bool bCanSkipInvisible)
{
    const SwFrame *pParent = GetParent( SwAccessibleChild(GetFrame()), IsInPagePreview() );
    ::rtl::Reference< SwAccessibleContext > xAccImpl(
            GetMap()->GetContextImpl( pParent, false ) );
    if( xAccImpl.is() )
        xAccImpl->DisposeChild(SwAccessibleChild(GetFrame()), bRecursive, bCanSkipInvisible);
    SwAccessibleContext::Dispose( bRecursive );
}

void SwAccessibleCell::InvalidatePosOrSize( const SwRect& rOldBox )
{
    const SwFrame *pParent = GetParent( SwAccessibleChild(GetFrame()), IsInPagePreview() );
    ::rtl::Reference< SwAccessibleContext > xAccImpl(
            GetMap()->GetContextImpl( pParent, false ) );
    if( xAccImpl.is() )
        xAccImpl->InvalidateChildPosOrSize( SwAccessibleChild(GetFrame()), rOldBox );
    SwAccessibleContext::InvalidatePosOrSize( rOldBox );
}

// XAccessibleValue

SwFrameFormat* SwAccessibleCell::GetTableBoxFormat() const
{
    assert(GetFrame());
    assert(GetFrame()->IsCellFrame());

    const SwCellFrame* pCellFrame = static_cast<const SwCellFrame*>( GetFrame() );
    return pCellFrame->GetTabBox()->GetFrameFormat();
}

//Implement TableCell currentValue
uno::Any SwAccessibleCell::getCurrentValue( )
{
    SolarMutexGuard aGuard;

    ThrowIfDisposed();

    return uno::Any( GetTableBoxFormat()->GetTableBoxValue().GetValue() );
}

sal_Bool SwAccessibleCell::setCurrentValue( const uno::Any& aNumber )
{
    SolarMutexGuard aGuard;

    ThrowIfDisposed();

    double fValue = 0;
    bool bValid = (aNumber >>= fValue);
    if( bValid )
    {
        SwTableBoxValue aValue( fValue );
        GetTableBoxFormat()->SetFormatAttr( aValue );
    }
    return bValid;
}

uno::Any SwAccessibleCell::getMaximumValue( )
{
    return uno::Any(DBL_MAX);
}

uno::Any SwAccessibleCell::getMinimumValue(  )
{
    return uno::Any(-DBL_MAX);
}

uno::Any SwAccessibleCell::getMinimumIncrement(  )
{
    return uno::Any();
}

OUString SAL_CALL SwAccessibleCell::getExtendedAttributes()
{
    SolarMutexGuard g;

    SwFrameFormat *pFrameFormat = GetTableBoxFormat();
    assert(pFrameFormat);

    const SwTableBoxFormula& tbl_formula = pFrameFormat->GetTableBoxFormula();

    OUString strFormula = tbl_formula.GetFormula()
                              .replaceAll(u"\\", u"\\\\")
                              .replaceAll(u";", u"\\;")
                              .replaceAll(u"=", u"\\=")
                              .replaceAll(u",", u"\\,")
                              .replaceAll(u":", u"\\:");
    return "Formula:" + strFormula + ";";
}

sal_Int32 SAL_CALL SwAccessibleCell::getBackground()
{
    SolarMutexGuard g;

    const SvxBrushItem &rBack = GetFrame()->GetAttrSet()->GetBackground();
    Color crBack = rBack.GetColor();

    if (COL_AUTO == crBack)
    {
        uno::Reference<XAccessible> xAccDoc = getAccessibleParent();
        if (xAccDoc.is())
        {
            uno::Reference<XAccessibleComponent> xComponentDoc(xAccDoc, uno::UNO_QUERY);
            if (xComponentDoc.is())
            {
                crBack = Color(ColorTransparency, xComponentDoc->getBackground());
            }
        }
    }
    return sal_Int32(crBack);
}

// XAccessibleSelection
void SwAccessibleCell::selectAccessibleChild(
    sal_Int64 nChildIndex )
{
    m_aSelectionHelper.selectAccessibleChild(nChildIndex);
}

sal_Bool SwAccessibleCell::isAccessibleChildSelected(
    sal_Int64 nChildIndex )
{
    return m_aSelectionHelper.isAccessibleChildSelected(nChildIndex);
}

void SwAccessibleCell::clearAccessibleSelection(  )
{
}

void SwAccessibleCell::selectAllAccessibleChildren(  )
{
    m_aSelectionHelper.selectAllAccessibleChildren();
}

sal_Int64 SwAccessibleCell::getSelectedAccessibleChildCount(  )
{
    return m_aSelectionHelper.getSelectedAccessibleChildCount();
}

uno::Reference<XAccessible> SwAccessibleCell::getSelectedAccessibleChild(
    sal_Int64 nSelectedChildIndex )
{
    return m_aSelectionHelper.getSelectedAccessibleChild(nSelectedChildIndex);
}

void SwAccessibleCell::deselectAccessibleChild(
    sal_Int64 nSelectedChildIndex )
{
    m_aSelectionHelper.deselectAccessibleChild(nSelectedChildIndex);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

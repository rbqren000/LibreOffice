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

#include <AccessibleDocument.hxx>
#include <AccessibleSpreadsheet.hxx>
#include <tabvwsh.hxx>
#include <AccessibilityHints.hxx>
#include <document.hxx>
#include <drwlayer.hxx>
#include <DrawModelBroadcaster.hxx>
#include <drawview.hxx>
#include <gridwin.hxx>
#include <AccessibleEditObject.hxx>
#include <userdat.hxx>
#include <scresid.hxx>
#include <strings.hrc>
#include <strings.hxx>
#include <markdata.hxx>

#include <com/sun/star/accessibility/AccessibleEventId.hpp>
#include <com/sun/star/accessibility/AccessibleStateType.hpp>
#include <com/sun/star/accessibility/AccessibleRelationType.hpp>
#include <com/sun/star/accessibility/AccessibleRole.hpp>
#include <com/sun/star/view/XSelectionSupplier.hpp>
#include <com/sun/star/drawing/ShapeCollection.hpp>
#include <com/sun/star/drawing/XShape.hpp>
#include <com/sun/star/drawing/XShapes.hpp>
#include <com/sun/star/lang/IndexOutOfBoundsException.hpp>
#include <o3tl/safeint.hxx>
#include <tools/gen.hxx>
#include <svx/svdpage.hxx>
#include <svx/svdobj.hxx>
#include <svx/ShapeTypeHandler.hxx>
#include <svx/AccessibleShape.hxx>
#include <svx/AccessibleShapeTreeInfo.hxx>
#include <svx/AccessibleShapeInfo.hxx>
#include <svx/IAccessibleParent.hxx>
#include <comphelper/sequence.hxx>
#include <sfx2/viewfrm.hxx>
#include <sfx2/docfile.hxx>
#include <toolkit/helper/vclunohelper.hxx>
#include <unotools/accessiblerelationsethelper.hxx>
#include <utility>
#include <vcl/svapp.hxx>
#include <vcl/unohelp.hxx>

#include <svx/AccessibleControlShape.hxx>
#include <svx/SvxShapeTypes.hxx>
#include <sfx2/objsh.hxx>
#include <editeng/editview.hxx>
#include <editeng/editeng.hxx>
#include <comphelper/processfactory.hxx>

#include <algorithm>

#include <scmod.hxx>

#ifdef indices
#undef indices
#endif

#ifdef extents
#undef extents
#endif

using namespace ::com::sun::star;
using namespace ::com::sun::star::accessibility;

namespace {

struct ScAccessibleShapeData
{
    ScAccessibleShapeData(css::uno::Reference< css::drawing::XShape > xShape_);
    ~ScAccessibleShapeData();
    mutable rtl::Reference< ::accessibility::AccessibleShape > pAccShape;
    std::optional<ScAddress> xRelationCell; // if it is NULL this shape is anchored on the table
    css::uno::Reference< css::drawing::XShape > xShape;
    bool                    bSelected;
    bool                    bSelectable;
    // cache these to make the sorting cheaper
    std::optional<sal_Int16> mxLayerID;
    std::optional<sal_Int32> mxZOrder;
};

}

ScAccessibleShapeData::ScAccessibleShapeData(css::uno::Reference< css::drawing::XShape > xShape_)
    : xShape(std::move(xShape_)),
    bSelected(false), bSelectable(true)
{
    static constexpr OUStringLiteral gsLayerId = u"LayerID";
    static constexpr OUStringLiteral gsZOrder = u"ZOrder";
    uno::Reference< beans::XPropertySet> xProps(xShape, uno::UNO_QUERY);
    if (xProps.is())
    {
        uno::Any aAny = xProps->getPropertyValue(gsLayerId);
        sal_Int16 nLayerID;
        if (aAny >>= nLayerID)
            mxLayerID = nLayerID;
        sal_Int32 nZOrder;
        aAny = xProps->getPropertyValue(gsZOrder);
        if (aAny >>= nZOrder)
            mxZOrder = nZOrder;
    }
}

ScAccessibleShapeData::~ScAccessibleShapeData()
{
    if (pAccShape.is())
    {
        pAccShape->dispose();
    }
}

namespace {

struct ScShapeDataLess
{
    static void ConvertLayerId(sal_Int16& rLayerID) // changes the number of the LayerId so it the accessibility order
    {
        // note: MSVC 2017 ICE's if this is written as "switch" so use "if"
        if (SC_LAYER_FRONT.get() == rLayerID)
        {
            rLayerID = 1;
        }
        else if (SC_LAYER_BACK.get() == rLayerID)
        {
            rLayerID = 0;
        }
        else if (SC_LAYER_INTERN.get() == rLayerID)
        {
            rLayerID = 2;
        }
        else if (SC_LAYER_CONTROLS.get() == rLayerID)
        {
            rLayerID = 3;
        }
    }
    static bool LessThanSheet(const ScAccessibleShapeData* pData)
    {
        bool bResult(false);
        if (pData->mxLayerID)
        {
            if (SdrLayerID(*pData->mxLayerID) == SC_LAYER_BACK)
                bResult = true;
        }
        return bResult;
    }
    bool operator()(const ScAccessibleShapeData* pData1, const ScAccessibleShapeData* pData2) const
    {
        bool bResult(false);
        if (pData1 && pData2)
        {
            if( pData1->mxLayerID && pData2->mxLayerID )
            {
                sal_Int16 nLayerID1 = *pData1->mxLayerID;
                sal_Int16 nLayerID2 = *pData2->mxLayerID;
                if (nLayerID1 == nLayerID2)
                {
                    if ( pData1->mxZOrder && pData2->mxZOrder )
                        bResult = (*pData1->mxZOrder < *pData2->mxZOrder);
                }
                else
                {
                    ConvertLayerId(nLayerID1);
                    ConvertLayerId(nLayerID2);
                    bResult = (nLayerID1 < nLayerID2);
                }
            }
        }
        else if (pData1 && !pData2)
            bResult = LessThanSheet(pData1);
        else if (!pData1 && pData2)
            bResult = !LessThanSheet(pData2);
        else
            bResult = false;
        return bResult;
    }
};

}

class ScChildrenShapes : public SfxListener,
    public ::accessibility::IAccessibleParent
{
public:
    ScChildrenShapes(ScAccessibleDocument* pAccessibleDocument, ScTabViewShell* pViewShell, ScSplitPos eSplitPos);
    virtual ~ScChildrenShapes() override;

    ///=====  SfxListener  =====================================================

    virtual void Notify( SfxBroadcaster& rBC, const SfxHint& rHint ) override;

    ///=====  IAccessibleParent  ===============================================

    virtual bool ReplaceChild (
        ::accessibility::AccessibleShape* pCurrentChild,
        const css::uno::Reference< css::drawing::XShape >& _rxShape,
        const tools::Long _nIndex,
        const ::accessibility::AccessibleShapeTreeInfo& _rShapeTreeInfo
    ) override;

    virtual ::accessibility::AccessibleControlShape* GetAccControlShapeFromModel
        (css::beans::XPropertySet* pSet) override;
    virtual ::accessibility::AccessibleShape*
        GetAccessibleCaption (const css::uno::Reference<css::drawing::XShape>& xShape) override;
    ///=====  Internal  ========================================================
    void SetDrawBroadcaster();

    sal_Int32 GetCount() const;
    rtl::Reference<::accessibility::AccessibleShape> Get(const ScAccessibleShapeData* pData) const;
    rtl::Reference<::accessibility::AccessibleShape> Get(sal_Int32 nIndex) const;
    uno::Reference< XAccessible > GetAt(const awt::Point& rPoint) const;

    // gets the index of the shape starting on 0 (without the index of the table)
    // returns the selected shape
    bool IsSelected(sal_Int32 nIndex,
        css::uno::Reference<css::drawing::XShape>& rShape) const;

    bool SelectionChanged();

    void Select(sal_Int32 nIndex);
    void DeselectAll(); // deselect also the table
    void SelectAll();
    sal_Int32 GetSelectedCount() const;
    rtl::Reference<::accessibility::AccessibleShape> GetSelected(sal_Int32 nSelectedChildIndex,
                                                                 bool bTabSelected) const;
    void Deselect(sal_Int32 nChildIndex);

    SdrPage* GetDrawPage() const;

    rtl::Reference<utl::AccessibleRelationSetHelper> GetRelationSet(const ScAddress* pAddress) const;

    void VisAreaChanged() const;
private:
    typedef std::vector<ScAccessibleShapeData*> SortedShapes;
    typedef std::unordered_map<css::uno::Reference< css::drawing::XShape >, ScAccessibleShapeData*> ShapesMap;

    mutable SortedShapes maZOrderedShapes; // a null pointer represents the sheet in the correct order
    mutable ShapesMap maShapesMap;
    mutable bool mbShapesNeedSorting; // set if maZOrderedShapes needs sorting

    mutable ::accessibility::AccessibleShapeTreeInfo maShapeTreeInfo;
    mutable css::uno::Reference<css::view::XSelectionSupplier> xSelectionSupplier;
    mutable sal_uInt32 mnShapesSelected;
    ScTabViewShell* mpViewShell;
    ScAccessibleDocument* mpAccessibleDocument;
    ScSplitPos meSplitPos;

    void FillShapes(std::vector < uno::Reference < drawing::XShape > >& rShapes) const;
    bool FindSelectedShapesChanges(const css::uno::Reference<css::drawing::XShapes>& xShapes) const;

    std::optional<ScAddress> GetAnchor(const uno::Reference<drawing::XShape>& xShape) const;
    rtl::Reference<utl::AccessibleRelationSetHelper> GetRelationSet(const ScAccessibleShapeData* pData) const;
    void SetAnchor(const uno::Reference<drawing::XShape>& xShape, ScAccessibleShapeData* pData) const;
    void AddShape(const uno::Reference<drawing::XShape>& xShape, bool bCommitChange) const;
    void RemoveShape(const uno::Reference<drawing::XShape>& xShape) const;

    bool FindShape(const uno::Reference<drawing::XShape>& xShape, SortedShapes::iterator& rItr) const;

    static sal_Int8 Compare(const ScAccessibleShapeData* pData1,
        const ScAccessibleShapeData* pData2);
};

ScChildrenShapes::ScChildrenShapes(ScAccessibleDocument* pAccessibleDocument, ScTabViewShell* pViewShell, ScSplitPos eSplitPos)
    :
    mbShapesNeedSorting(false),
    mnShapesSelected(0),
    mpViewShell(pViewShell),
    mpAccessibleDocument(pAccessibleDocument),
    meSplitPos(eSplitPos)
{
    if (mpViewShell)
    {
        SfxViewFrame& rViewFrame = mpViewShell->GetViewFrame();
        xSelectionSupplier = uno::Reference<view::XSelectionSupplier>(rViewFrame.GetFrame().GetController(), uno::UNO_QUERY);
        if (xSelectionSupplier.is())
        {
            xSelectionSupplier->addSelectionChangeListener(mpAccessibleDocument);
            uno::Reference<drawing::XShapes> xShapes(mpViewShell->getSelectedXShapes());
            if (xShapes.is())
                mnShapesSelected = xShapes->getCount();
        }
    }

    maZOrderedShapes.push_back(nullptr); // add an element which represents the table

    GetCount(); // fill list with filtered shapes (no internal shapes)

    if (mnShapesSelected)
    {
        //set flag on every selected shape
        if (!xSelectionSupplier.is())
            throw uno::RuntimeException("Could not get selected shapes. Null reference to xSelectionSupplier in ScChildrenShapes::ScChildrenShapes.");

        uno::Reference<drawing::XShapes> xShapes(mpViewShell->getSelectedXShapes());
        if (xShapes.is())
            FindSelectedShapesChanges(xShapes);
    }
    if (!pViewShell)
        return;

    ScViewData& rViewData = pViewShell->GetViewData();
    SfxBroadcaster* pDrawBC = rViewData.GetDocument().GetDrawBroadcaster();
    if (pDrawBC)
    {
        StartListening(*pDrawBC);

        maShapeTreeInfo.SetModelBroadcaster( new ScDrawModelBroadcaster(rViewData.GetDocument().GetDrawLayer()) );
        maShapeTreeInfo.SetSdrView(rViewData.GetScDrawView());
        maShapeTreeInfo.SetController(nullptr);
        maShapeTreeInfo.SetWindow(pViewShell->GetWindowByPos(meSplitPos));
        maShapeTreeInfo.SetViewForwarder(mpAccessibleDocument);
    }
}

ScChildrenShapes::~ScChildrenShapes()
{
    for (ScAccessibleShapeData* pShapeData : maZOrderedShapes)
        delete pShapeData;
    if (mpViewShell)
    {
        SfxBroadcaster* pDrawBC = mpViewShell->GetViewData().GetDocument().GetDrawBroadcaster();
        if (pDrawBC)
            EndListening(*pDrawBC);
    }
    if (mpAccessibleDocument && xSelectionSupplier.is())
        xSelectionSupplier->removeSelectionChangeListener(mpAccessibleDocument);
}

void ScChildrenShapes::SetDrawBroadcaster()
{
    if (!mpViewShell)
        return;

    ScViewData& rViewData = mpViewShell->GetViewData();
    SfxBroadcaster* pDrawBC = rViewData.GetDocument().GetDrawBroadcaster();
    if (pDrawBC)
    {
        StartListening(*pDrawBC, DuplicateHandling::Prevent);

        maShapeTreeInfo.SetModelBroadcaster( new ScDrawModelBroadcaster(rViewData.GetDocument().GetDrawLayer()) );
        maShapeTreeInfo.SetSdrView(rViewData.GetScDrawView());
        maShapeTreeInfo.SetController(nullptr);
        maShapeTreeInfo.SetWindow(mpViewShell->GetWindowByPos(meSplitPos));
        maShapeTreeInfo.SetViewForwarder(mpAccessibleDocument);
    }
}

void ScChildrenShapes::Notify(SfxBroadcaster&, const SfxHint& rHint)
{
    if (rHint.GetId() != SfxHintId::ThisIsAnSdrHint)
        return;
    const SdrHint* pSdrHint = static_cast<const SdrHint*>(&rHint);

    SdrObject* pObj = const_cast<SdrObject*>(pSdrHint->GetObject());
    if (!(pObj && /*(pObj->GetLayer() != SC_LAYER_INTERN) && */(pObj->getSdrPageFromSdrObject() == GetDrawPage()) &&
        (pObj->getSdrPageFromSdrObject() == pObj->getParentSdrObjListFromSdrObject())) ) //only do something if the object lies direct on the page
        return;

    switch (pSdrHint->GetKind())
    {
        case SdrHintKind::ObjectChange :         // object changed
        {
            uno::Reference<drawing::XShape> xShape (pObj->getUnoShape(), uno::UNO_QUERY);
            if (xShape.is())
            {
                mbShapesNeedSorting = true; // sort, because the z index or layer could be changed
                auto it = maShapesMap.find(xShape);
                if (it != maShapesMap.end())
                    SetAnchor(xShape, it->second);
            }
        }
        break;
        case SdrHintKind::ObjectInserted :    // new drawing object inserted
        {
            uno::Reference<drawing::XShape> xShape (pObj->getUnoShape(), uno::UNO_QUERY);
            if (xShape.is())
                AddShape(xShape, true);
        }
        break;
        case SdrHintKind::ObjectRemoved :     // Removed drawing object from list
        {
            uno::Reference<drawing::XShape> xShape (pObj->getUnoShape(), uno::UNO_QUERY);
            if (xShape.is())
                RemoveShape(xShape);
        }
        break;
        default :
        {
            // other events are not interesting
        }
        break;
    }
}

bool ScChildrenShapes::ReplaceChild (::accessibility::AccessibleShape* pCurrentChild,
        const css::uno::Reference< css::drawing::XShape >& _rxShape,
        const tools::Long /*_nIndex*/, const ::accessibility::AccessibleShapeTreeInfo& _rShapeTreeInfo)
{
    // create the new child
    rtl::Reference< ::accessibility::AccessibleShape > pReplacement(::accessibility::ShapeTypeHandler::Instance().CreateAccessibleObject (
        ::accessibility::AccessibleShapeInfo ( _rxShape, pCurrentChild->getAccessibleParent(), this ),
        _rShapeTreeInfo
    ));

    bool bResult(false);
    if (pReplacement.is())
    {
        OSL_ENSURE(pCurrentChild->GetXShape().get() == pReplacement->GetXShape().get(), "XShape changes and should be inserted sorted");
        auto it = maShapesMap.find(pCurrentChild->GetXShape());
        if (it != maShapesMap.end() && it->second->pAccShape.is())
        {
            OSL_ENSURE(it->second->pAccShape == pCurrentChild, "wrong child found");
            // child is gone - event
            mpAccessibleDocument->CommitChange(AccessibleEventId::CHILD,
                                               uno::Any(uno::Reference<XAccessible>(pCurrentChild)),
                                               uno::Any());

            pCurrentChild->dispose();
        }

        // Init after above possible pCurrentChild->dispose so we don't trigger the assert
        // ScDrawModelBroadcaster::addShapeEventListener of duplicate listeners
        pReplacement->Init();

        if (it != maShapesMap.end())
        {
            it->second->pAccShape = pReplacement;
            // child is new - event
            mpAccessibleDocument->CommitChange(AccessibleEventId::CHILD, uno::Any(),
                                               uno::Any(uno::Reference<XAccessible>(pReplacement)));
            bResult = true;
        }
    }
    return bResult;
}

::accessibility::AccessibleControlShape * ScChildrenShapes::GetAccControlShapeFromModel(css::beans::XPropertySet* pSet)
{
    GetCount(); // populate
    for (ScAccessibleShapeData* pShape : maZOrderedShapes)
    {
        if (pShape)
        {
            rtl::Reference< ::accessibility::AccessibleShape > pAccShape(pShape->pAccShape);
            if (pAccShape.is() && ::accessibility::ShapeTypeHandler::Instance().GetTypeId (pAccShape->GetXShape()) == ::accessibility::DRAWING_CONTROL)
            {
                ::accessibility::AccessibleControlShape *pCtlAccShape = static_cast < ::accessibility::AccessibleControlShape* >(pAccShape.get());
                if (pCtlAccShape && pCtlAccShape->GetControlModel() == pSet)
                    return pCtlAccShape;
            }
        }
    }
    return nullptr;
}

::accessibility::AccessibleShape*
ScChildrenShapes::GetAccessibleCaption (const css::uno::Reference < css::drawing::XShape>& xShape)
{
    GetCount(); // populate
    auto it = maShapesMap.find(xShape);
    if (it == maShapesMap.end())
        return nullptr;
    ScAccessibleShapeData* pShape = it->second;
    return pShape->pAccShape.get();
}

sal_Int32 ScChildrenShapes::GetCount() const
{
    SdrPage* pDrawPage = GetDrawPage();
    if (pDrawPage && (maZOrderedShapes.size() == 1)) // the table is always in
    {
        size_t nSdrObjCount = pDrawPage->GetObjCount();
        maZOrderedShapes.reserve(nSdrObjCount + 1); // the table is always in
        for (const rtl::Reference<SdrObject>& pObj : *pDrawPage)
        {
            uno::Reference< drawing::XShape > xShape (pObj->getUnoShape(), uno::UNO_QUERY);
            AddShape(xShape, false); //inserts in the correct order
        }
    }
    return maZOrderedShapes.size();
}

rtl::Reference<::accessibility::AccessibleShape>
ScChildrenShapes::Get(const ScAccessibleShapeData* pData) const
{
    if (!pData)
        return nullptr;

    if (!pData->pAccShape.is())
    {
        ::accessibility::ShapeTypeHandler& rShapeHandler = ::accessibility::ShapeTypeHandler::Instance();
        ::accessibility::AccessibleShapeInfo aShapeInfo(pData->xShape, mpAccessibleDocument, const_cast<ScChildrenShapes*>(this));
        pData->pAccShape = rShapeHandler.CreateAccessibleObject(
            aShapeInfo, maShapeTreeInfo);
        if (pData->pAccShape.is())
        {
            pData->pAccShape->Init();
            if (pData->bSelected)
                pData->pAccShape->SetState(AccessibleStateType::SELECTED);
            if (!pData->bSelectable)
                pData->pAccShape->ResetState(AccessibleStateType::SELECTABLE);
            pData->pAccShape->SetRelationSet(GetRelationSet(pData));
        }
    }
    return pData->pAccShape;
 }

rtl::Reference<::accessibility::AccessibleShape> ScChildrenShapes::Get(sal_Int32 nIndex) const
{
    if (maZOrderedShapes.size() <= 1)
        GetCount(); // fill list with filtered shapes (no internal shapes)

    if (mbShapesNeedSorting)
    {
        std::sort(maZOrderedShapes.begin(), maZOrderedShapes.end(), ScShapeDataLess());
        mbShapesNeedSorting = false;
    }

    if (o3tl::make_unsigned(nIndex) >= maZOrderedShapes.size())
        return nullptr;

    return Get(maZOrderedShapes[nIndex]);
}

uno::Reference< XAccessible > ScChildrenShapes::GetAt(const awt::Point& rPoint) const
{
    uno::Reference<XAccessible> xAccessible;
    if(mpViewShell)
    {
        if (mbShapesNeedSorting)
        {
            std::sort(maZOrderedShapes.begin(), maZOrderedShapes.end(), ScShapeDataLess());
            mbShapesNeedSorting = false;
        }

        sal_Int32 i(maZOrderedShapes.size() - 1);
        bool bFound(false);
        while (!bFound && i >= 0)
        {
            ScAccessibleShapeData* pShape = maZOrderedShapes[i];
            if (pShape)
            {
                if (!pShape->pAccShape.is())
                    Get(pShape);

                if (pShape->pAccShape.is())
                {
                    Point aPoint(vcl::unohelper::ConvertToVCLPoint(rPoint));
                    aPoint
                        -= vcl::unohelper::ConvertToVCLRect(pShape->pAccShape->getBounds()).TopLeft();
                    if (pShape->pAccShape->containsPoint(vcl::unohelper::ConvertToAWTPoint(aPoint)))
                    {
                        xAccessible = pShape->pAccShape.get();
                        bFound = true;
                    }
                }
                else
                {
                    OSL_FAIL("I should have an accessible shape now!");
                }
            }
            else
                bFound = true; // this is the sheet and it lies before the rest of the shapes which are background shapes

            --i;
        }
    }
    return xAccessible;
}

bool ScChildrenShapes::IsSelected(sal_Int32 nIndex,
                        uno::Reference<drawing::XShape>& rShape) const
{
    bool bResult (false);
    if (maZOrderedShapes.size() <= 1)
        GetCount(); // fill list with filtered shapes (no internal shapes)

    if (!xSelectionSupplier.is())
        throw uno::RuntimeException("Could not get selected shapes. Null reference to xSelectionSupplier in ScChildrenShapes::IsSelected.");

    if (mbShapesNeedSorting)
    {
        std::sort(maZOrderedShapes.begin(), maZOrderedShapes.end(), ScShapeDataLess());
        mbShapesNeedSorting = false;
    }

    if (!maZOrderedShapes[nIndex])
        return false;

    bResult = maZOrderedShapes[nIndex]->bSelected;
    rShape = maZOrderedShapes[nIndex]->xShape;

#if OSL_DEBUG_LEVEL > 0 // test whether it is truly selected by a slower method
    uno::Reference< drawing::XShape > xReturnShape;
    bool bDebugResult(false);
    uno::Reference<drawing::XShapes> xShapes(mpViewShell->getSelectedXShapes());

    if (xShapes.is())
    {
        sal_Int32 nCount(xShapes->getCount());
        if (nCount)
        {
            uno::Reference< drawing::XShape > xShape;
            uno::Reference< drawing::XShape > xIndexShape = maZOrderedShapes[nIndex]->xShape;
            sal_Int32 i(0);
            while (!bDebugResult && (i < nCount))
            {
                xShapes->getByIndex(i) >>= xShape;
                if (xShape.is() && (xIndexShape.get() == xShape.get()))
                {
                    bDebugResult = true;
                    xReturnShape = xShape;
                }
                else
                    ++i;
            }
        }
    }
    OSL_ENSURE((bResult == bDebugResult) && ((bResult && (rShape.get() == xReturnShape.get())) || !bResult), "found the wrong shape or result");
#endif

    return bResult;
}

bool ScChildrenShapes::SelectionChanged()
{
    bool bResult(false);
    if (!xSelectionSupplier.is())
        throw uno::RuntimeException("Could not get selected shapes. Null reference to xSelectionSupplier in ScChildrenShapes::SelectionChanged.");

    uno::Reference<drawing::XShapes> xShapes(mpViewShell->getSelectedXShapes());

    bResult = FindSelectedShapesChanges(xShapes);

    return bResult;
}

void ScChildrenShapes::Select(sal_Int32 nIndex)
{
    if (maZOrderedShapes.size() <= 1)
        GetCount(); // fill list with filtered shapes (no internal shapes)

    if (!xSelectionSupplier.is())
        throw uno::RuntimeException("Could not get selected shapes. Null reference to xSelectionSupplier in ScChildrenShapes::Select.");

    if (mbShapesNeedSorting)
    {
        std::sort(maZOrderedShapes.begin(), maZOrderedShapes.end(), ScShapeDataLess());
        mbShapesNeedSorting = false;
    }

    if (!maZOrderedShapes[nIndex])
        return;

    uno::Reference<drawing::XShape> xShape;
    if (IsSelected(nIndex, xShape) || !maZOrderedShapes[nIndex]->bSelectable)
        return;

    uno::Reference<drawing::XShapes> xShapes(mpViewShell->getSelectedXShapes());

    if (!xShapes.is())
        xShapes = drawing::ShapeCollection::create(
                comphelper::getProcessComponentContext());

    xShapes->add(maZOrderedShapes[nIndex]->xShape);

    try
    {
        xSelectionSupplier->select(uno::Any(xShapes));
        maZOrderedShapes[nIndex]->bSelected = true;
        if (maZOrderedShapes[nIndex]->pAccShape.is())
            maZOrderedShapes[nIndex]->pAccShape->SetState(AccessibleStateType::SELECTED);
    }
    catch (lang::IllegalArgumentException&)
    {
    }
}

void ScChildrenShapes::DeselectAll()
{
    if (!xSelectionSupplier.is())
        throw uno::RuntimeException("Could not get selected shapes. Null reference to xSelectionSupplier in ScChildrenShapes::DeselectAll.");

    bool bSomethingSelected(true);
    try
    {
        xSelectionSupplier->select(uno::Any()); //deselects all
    }
    catch (lang::IllegalArgumentException&)
    {
        OSL_FAIL("nothing selected before");
        bSomethingSelected = false;
    }

    if (bSomethingSelected)
        for (ScAccessibleShapeData* pAccShapeData : maZOrderedShapes)
            if (pAccShapeData)
            {
                pAccShapeData->bSelected = false;
                if (pAccShapeData->pAccShape.is())
                    pAccShapeData->pAccShape->ResetState(AccessibleStateType::SELECTED);
            }
};


void ScChildrenShapes::SelectAll()
{
    if (!xSelectionSupplier.is())
        throw uno::RuntimeException("Could not get selected shapes. Null reference to xSelectionSupplier in ScChildrenShapes::SelectAll.");

    if (maZOrderedShapes.size() <= 1)
        GetCount(); // fill list with filtered shapes (no internal shapes)

    if (maZOrderedShapes.size() <= 1)
        return;

    uno::Reference<drawing::XShapes> xShapes = drawing::ShapeCollection::create(
            comphelper::getProcessComponentContext());

    try
    {
        for (ScAccessibleShapeData* pAccShapeData : maZOrderedShapes)
        {
            if (pAccShapeData && pAccShapeData->bSelectable)
            {
                pAccShapeData->bSelected = true;
                if (pAccShapeData->pAccShape.is())
                    pAccShapeData->pAccShape->SetState(AccessibleStateType::SELECTED);
                if (xShapes.is())
                    xShapes->add(pAccShapeData->xShape);
            }
        }
        xSelectionSupplier->select(uno::Any(xShapes));
    }
    catch (lang::IllegalArgumentException&)
    {
        SelectionChanged(); // find all selected shapes and set the flags
    }
}

void ScChildrenShapes::FillShapes(std::vector < uno::Reference < drawing::XShape > >& rShapes) const
{
    uno::Reference<drawing::XShapes> xShapes(mpViewShell->getSelectedXShapes());
    if (xShapes.is())
    {
        sal_uInt32 nCount(xShapes->getCount());
        for (sal_uInt32 i = 0; i < nCount; ++i)
        {
            uno::Reference<drawing::XShape> xShape;
            xShapes->getByIndex(i) >>= xShape;
            if (xShape.is())
                rShapes.push_back(xShape);
        }
    }
}

sal_Int32 ScChildrenShapes::GetSelectedCount() const
{
    if (!xSelectionSupplier.is())
        throw uno::RuntimeException("Could not get selected shapes. Null reference to xSelectionSupplier in ScChildrenShapes::GetSelectedCount.");

    std::vector < uno::Reference < drawing::XShape > > aShapes;
    FillShapes(aShapes);

    return aShapes.size();
}

rtl::Reference<::accessibility::AccessibleShape>
ScChildrenShapes::GetSelected(sal_Int32 nSelectedChildIndex, bool bTabSelected) const
{
    rtl::Reference<::accessibility::AccessibleShape> xAccessible;

    if (maZOrderedShapes.size() <= 1)
        GetCount(); // fill list with shapes

    if (!bTabSelected)
    {
        std::vector < uno::Reference < drawing::XShape > > aShapes;
        FillShapes(aShapes);

        if (nSelectedChildIndex < 0 || o3tl::make_unsigned(nSelectedChildIndex) >= aShapes.size())
            return xAccessible;

        SortedShapes::iterator aItr;
        if (FindShape(aShapes[nSelectedChildIndex], aItr))
            xAccessible = Get(*aItr);
    }
    else
    {
        if (mbShapesNeedSorting)
        {
            std::sort(maZOrderedShapes.begin(), maZOrderedShapes.end(), ScShapeDataLess());
            mbShapesNeedSorting = false;
        }
        for(const auto& rpShape : maZOrderedShapes)
        {
            if (!rpShape || rpShape->bSelected)
            {
                if (nSelectedChildIndex == 0)
                {
                    if (rpShape)
                        xAccessible = rpShape->pAccShape.get();
                    break;
                }
                else
                    --nSelectedChildIndex;
            }
        }
    }

    return xAccessible;
}

void ScChildrenShapes::Deselect(sal_Int32 nChildIndex)
{
    uno::Reference<drawing::XShape> xShape;
    if (!IsSelected(nChildIndex, xShape)) // returns false if it is the sheet
        return;

    if (!xShape.is())
        return;

    uno::Reference<drawing::XShapes> xShapes(mpViewShell->getSelectedXShapes());
    if (xShapes.is())
        xShapes->remove(xShape);

    try
    {
        xSelectionSupplier->select(uno::Any(xShapes));
    }
    catch (lang::IllegalArgumentException&)
    {
        OSL_FAIL("something not selectable");
    }

    maZOrderedShapes[nChildIndex]->bSelected = false;
    if (maZOrderedShapes[nChildIndex]->pAccShape.is())
        maZOrderedShapes[nChildIndex]->pAccShape->ResetState(AccessibleStateType::SELECTED);
}

SdrPage* ScChildrenShapes::GetDrawPage() const
{
    SCTAB nTab(mpAccessibleDocument->getVisibleTable());
    SdrPage* pDrawPage = nullptr;
    if (mpViewShell)
    {
        ScDocument& rDoc = mpViewShell->GetViewData().GetDocument();
        if (ScDrawLayer* pDrawLayer = rDoc.GetDrawLayer())
        {
            if (pDrawLayer->HasObjects() && (pDrawLayer->GetPageCount() > nTab))
                pDrawPage = pDrawLayer->GetPage(static_cast<sal_uInt16>(static_cast<sal_Int16>(nTab)));
        }
    }
    return pDrawPage;
}

rtl::Reference<utl::AccessibleRelationSetHelper> ScChildrenShapes::GetRelationSet(const ScAddress* pAddress) const
{
    rtl::Reference<utl::AccessibleRelationSetHelper> pRelationSet;
    for (const ScAccessibleShapeData* pAccShapeData : maZOrderedShapes)
    {
        if (pAccShapeData &&
            ((!pAccShapeData->xRelationCell && !pAddress) ||
            (pAccShapeData->xRelationCell && pAddress && (*(pAccShapeData->xRelationCell) == *pAddress))))
        {
            if (!pRelationSet)
                pRelationSet = new utl::AccessibleRelationSetHelper();

            AccessibleRelation aRelation;
            aRelation.TargetSet = { Get(pAccShapeData) };
            aRelation.RelationType = AccessibleRelationType_CONTROLLER_FOR;

            pRelationSet->AddRelation(aRelation);
        }
    }
    return pRelationSet;
}

bool ScChildrenShapes::FindSelectedShapesChanges(const uno::Reference<drawing::XShapes>& xShapes) const
{
    bool bResult(false);
    SortedShapes aShapesList;
    if (xShapes.is())
    {
        mnShapesSelected = xShapes->getCount();
        for (sal_uInt32 i = 0; i < mnShapesSelected; ++i)
        {
            uno::Reference< drawing::XShape > xShape;
            xShapes->getByIndex(i) >>= xShape;
            if (xShape.is())
            {
                ScAccessibleShapeData* pShapeData = new ScAccessibleShapeData(xShape);
                aShapesList.push_back(pShapeData);
            }
        }
    }
    else
        mnShapesSelected = 0;
    SdrObject *pFocusedObj = nullptr;
    if( mnShapesSelected == 1 && aShapesList.size() == 1)
    {
        pFocusedObj = SdrObject::getSdrObjectFromXShape(aShapesList[0]->xShape);
    }
    std::sort(aShapesList.begin(), aShapesList.end(), ScShapeDataLess());
    SortedShapes vecSelectedShapeAdd;
    SortedShapes vecSelectedShapeRemove;
    bool bHasSelect=false;
    SortedShapes::iterator aXShapesItr(aShapesList.begin());
    SortedShapes::const_iterator aXShapesEndItr(aShapesList.end());
    SortedShapes::iterator aDataItr(maZOrderedShapes.begin());
    SortedShapes::const_iterator aDataEndItr(maZOrderedShapes.end());
    SortedShapes::const_iterator aFocusedItr = aDataEndItr;
    while(aDataItr != aDataEndItr)
    {
        if (*aDataItr) // is it really a shape or only the sheet
        {
            sal_Int8 nComp(0);
            if (aXShapesItr == aXShapesEndItr)
                nComp = -1; // simulate that the Shape is lower, so the selection state will be removed
            else
                nComp = Compare(*aDataItr, *aXShapesItr);
            if (nComp == 0)
            {
                if (!(*aDataItr)->bSelected)
                {
                    (*aDataItr)->bSelected = true;
                    if ((*aDataItr)->pAccShape.is())
                    {
                        (*aDataItr)->pAccShape->SetState(AccessibleStateType::SELECTED);
                        (*aDataItr)->pAccShape->SetState(AccessibleStateType::FOCUSED);
                        bResult = true;
                        vecSelectedShapeAdd.push_back(*aDataItr);
                    }
                    aFocusedItr = aDataItr;
                }
                else
                {
                     bHasSelect = true;
                }
                ++aDataItr;
                ++aXShapesItr;
            }
            else if (nComp < 0)
            {
                if ((*aDataItr)->bSelected)
                {
                    (*aDataItr)->bSelected = false;
                    if ((*aDataItr)->pAccShape.is())
                    {
                        (*aDataItr)->pAccShape->ResetState(AccessibleStateType::SELECTED);
                        (*aDataItr)->pAccShape->ResetState(AccessibleStateType::FOCUSED);
                        bResult = true;
                        vecSelectedShapeRemove.push_back(*aDataItr);
                    }
                }
                ++aDataItr;
            }
            else
            {
                OSL_FAIL("here is a selected shape which is not in the childlist");
                ++aXShapesItr;
                --mnShapesSelected;
            }
        }
        else
            ++aDataItr;
    }
    bool bWinFocus=false;
    if (mpViewShell)
    {
        ScGridWindow* pWin = static_cast<ScGridWindow*>(mpViewShell->GetWindowByPos(meSplitPos));
        if (pWin)
        {
            bWinFocus = pWin->HasFocus();
        }
    }
    const SdrMarkList* pMarkList = nullptr;
    SdrObject* pMarkedObj = nullptr;
    bool bIsFocuseMarked = true;
    if( mpViewShell && mnShapesSelected == 1 && bWinFocus)
    {
        ScDrawView* pScDrawView = mpViewShell->GetViewData().GetScDrawView();
        if( pScDrawView )
        {
            pMarkList = &(pScDrawView->GetMarkedObjectList());
            if( pMarkList->GetMarkCount() == 1 )
            {
                pMarkedObj = pMarkList->GetMark(0)->GetMarkedSdrObj();
                uno::Reference< drawing::XShape > xMarkedXShape (pMarkedObj->getUnoShape(), uno::UNO_QUERY);
                if( aFocusedItr != aDataEndItr &&
                    (*aFocusedItr)->xShape.is() &&
                    xMarkedXShape.is() &&
                    (*aFocusedItr)->xShape != xMarkedXShape )
                    bIsFocuseMarked = false;
            }
        }
    }
    //if ((aFocusedItr != aDataEndItr) && (*aFocusedItr)->pAccShape.is() && (mnShapesSelected == 1))
    if ( bIsFocuseMarked && (aFocusedItr != aDataEndItr) && (*aFocusedItr)->pAccShape.is() && (mnShapesSelected == 1) && bWinFocus)
    {
        (*aFocusedItr)->pAccShape->SetState(AccessibleStateType::FOCUSED);
    }
    else if( pFocusedObj && bWinFocus && pMarkList && pMarkList->GetMarkCount() == 1 && mnShapesSelected == 1 )
    {
        if( pMarkedObj )
        {
            uno::Reference< drawing::XShape > xMarkedXShape (pMarkedObj->getUnoShape(), uno::UNO_QUERY);
            SdrObject* pUpObj = pMarkedObj->getParentSdrObjectFromSdrObject();

            if( pMarkedObj == pFocusedObj && pUpObj )
            {
                uno::Reference< drawing::XShape > xUpGroupXShape (pUpObj->getUnoShape(), uno::UNO_QUERY);
                ::accessibility::AccessibleShape* pAccGroupShape =
                    const_cast<ScChildrenShapes*>(this)->GetAccessibleCaption( xUpGroupXShape );
                if( pAccGroupShape )
                {
                    sal_Int64 nCount =  pAccGroupShape->getAccessibleChildCount();
                    for( sal_Int64 i = 0; i < nCount; i++ )
                    {
                        uno::Reference<XAccessible> xAccShape = pAccGroupShape->getAccessibleChild(i);
                        if (xAccShape.is())
                        {
                            ::accessibility::AccessibleShape* pChildAccShape =  static_cast< ::accessibility::AccessibleShape* >(xAccShape.get());
                            uno::Reference< drawing::XShape > xChildShape = pChildAccShape->GetXShape();
                            if (xChildShape == xMarkedXShape)
                            {
                                pChildAccShape->SetState(AccessibleStateType::FOCUSED);
                            }
                            else
                            {
                                pChildAccShape->ResetState(AccessibleStateType::FOCUSED);
                            }
                        }
                    }
                }
            }
        }
    }
    if (vecSelectedShapeAdd.size() >= 10 )
    {
        mpAccessibleDocument->CommitChange(AccessibleEventId::SELECTION_CHANGED_WITHIN, uno::Any(),
                                           uno::Any());
    }
    else
    {
        for (const auto& rpShape : vecSelectedShapeAdd)
        {
            sal_Int16 nEventId;
            if (bHasSelect)
                nEventId = AccessibleEventId::SELECTION_CHANGED_ADD;
            else
                nEventId = AccessibleEventId::SELECTION_CHANGED;
            mpAccessibleDocument->CommitChange(
                nEventId, uno::Any(), uno::Any(uno::Reference<XAccessible>(rpShape->pAccShape)));
        }
    }
    for (const auto& rpShape : vecSelectedShapeRemove)
    {
        mpAccessibleDocument->CommitChange(
            AccessibleEventId::SELECTION_CHANGED_REMOVE, uno::Any(),
            uno::Any(uno::Reference<XAccessible>(rpShape->pAccShape)));
    }
    for(ScAccessibleShapeData*& pShapeData : aShapesList)
    {
        delete pShapeData;
        pShapeData = nullptr;
    }
    return bResult;
}

std::optional<ScAddress> ScChildrenShapes::GetAnchor(const uno::Reference<drawing::XShape>& xShape) const
{
    if (mpViewShell)
    {
        SdrObject* pSdrObj = SdrObject::getSdrObjectFromXShape(xShape);
        uno::Reference<beans::XPropertySet> xShapeProp(xShape, uno::UNO_QUERY);
        if (pSdrObj && xShapeProp.is())
        {
            if (ScDrawObjData *pAnchor = ScDrawLayer::GetObjData(pSdrObj))
                return std::optional<ScAddress>(pAnchor->maStart);
        }
    }

    return std::optional<ScAddress>();
}

rtl::Reference<utl::AccessibleRelationSetHelper> ScChildrenShapes::GetRelationSet(const ScAccessibleShapeData* pData) const
{
    rtl::Reference<utl::AccessibleRelationSetHelper> pRelationSet = new utl::AccessibleRelationSetHelper();

    if (pData && mpAccessibleDocument)
    {
        uno::Reference<XAccessible> xAccessible = mpAccessibleDocument->GetAccessibleSpreadsheet(); // should be the current table
        if (pData->xRelationCell && xAccessible.is())
        {
            sal_Int32 nRow = pData->xRelationCell->Row();
            sal_Int32 nColumn = pData->xRelationCell->Col();
            bool bPositionUnset = nRow == -1 && nColumn == -1;
            if (!bPositionUnset)
            {
                uno::Reference<XAccessibleTable> xAccTable(xAccessible->getAccessibleContext(), uno::UNO_QUERY);
                if (xAccTable.is())
                    xAccessible = xAccTable->getAccessibleCellAt(nRow, nColumn);
            }
        }
        AccessibleRelation aRelation;
        aRelation.TargetSet = { xAccessible };
        aRelation.RelationType = AccessibleRelationType_CONTROLLED_BY;
        pRelationSet->AddRelation(aRelation);
    }

    return pRelationSet;
}

void ScChildrenShapes::SetAnchor(const uno::Reference<drawing::XShape>& xShape, ScAccessibleShapeData* pData) const
{
    if (pData)
    {
        std::optional<ScAddress> xAddress = GetAnchor(xShape);
        if ((xAddress && pData->xRelationCell && (*xAddress != *(pData->xRelationCell))) ||
            (!xAddress && pData->xRelationCell) || (xAddress && !pData->xRelationCell))
        {
            pData->xRelationCell = std::move(xAddress);
            if (pData->pAccShape.is())
                pData->pAccShape->SetRelationSet(GetRelationSet(pData));
        }
    }
}

void ScChildrenShapes::AddShape(const uno::Reference<drawing::XShape>& xShape, bool bCommitChange) const
{
    assert( maShapesMap.find(xShape) == maShapesMap.end());

    ScAccessibleShapeData* pShape = new ScAccessibleShapeData(xShape);
    maZOrderedShapes.push_back(pShape);
    mbShapesNeedSorting = true;
    maShapesMap[xShape] = pShape;
    SetAnchor(xShape, pShape);

    uno::Reference< beans::XPropertySet > xShapeProp(xShape, uno::UNO_QUERY);
    if (xShapeProp.is())
    {
        uno::Any aPropAny = xShapeProp->getPropertyValue(u"LayerID"_ustr);
        sal_Int16 nLayerID = 0;
        if( aPropAny >>= nLayerID )
        {
            if( (SdrLayerID(nLayerID) == SC_LAYER_INTERN) || (SdrLayerID(nLayerID) == SC_LAYER_HIDDEN) )
                pShape->bSelectable = false;
            else
                pShape->bSelectable = true;
        }
    }

    if (!xSelectionSupplier.is())
        throw uno::RuntimeException("Could not get selected shapes. Null reference to xSelectionSupplier in ScChildrenShapes::AddShape.");

    uno::Reference<drawing::XShapes> xShapes(mpViewShell->getSelectedXShapes());
    uno::Reference<container::XEnumerationAccess> xEnumAcc(xShapes, uno::UNO_QUERY);
    if (xEnumAcc.is())
    {
        uno::Reference<container::XEnumeration> xEnum = xEnumAcc->createEnumeration();
        if (xEnum.is())
        {
            uno::Reference<drawing::XShape> xSelectedShape;
            bool bFound(false);
            while (!bFound && xEnum->hasMoreElements())
            {
                xEnum->nextElement() >>= xSelectedShape;
                if (xShape.is() && (xShape.get() == xSelectedShape.get()))
                {
                    pShape->bSelected = true;
                    bFound = true;
                }
            }
        }
    }
    if (mpAccessibleDocument && bCommitChange)
    {
        // new child - event
        mpAccessibleDocument->CommitChange(AccessibleEventId::CHILD, uno::Any(),
                                           uno::Any(uno::Reference<XAccessible>(Get(pShape))));
    }
}

void ScChildrenShapes::RemoveShape(const uno::Reference<drawing::XShape>& xShape) const
{
    if (mbShapesNeedSorting)
    {
        std::sort(maZOrderedShapes.begin(), maZOrderedShapes.end(), ScShapeDataLess());
        mbShapesNeedSorting = false;
    }
    SortedShapes::iterator aItr;
    if (FindShape(xShape, aItr))
    {
        if (mpAccessibleDocument)
        {
            rtl::Reference<::accessibility::AccessibleShape> xOldAccessible(Get(*aItr));

            delete *aItr;
            maShapesMap.erase((*aItr)->xShape);
            maZOrderedShapes.erase(aItr);

            // child is gone - event
            mpAccessibleDocument->CommitChange(
                AccessibleEventId::CHILD, uno::Any(uno::Reference<XAccessible>(xOldAccessible)),
                uno::Any());
        }
        else
        {
            delete *aItr;
            maShapesMap.erase((*aItr)->xShape);
            maZOrderedShapes.erase(aItr);
        }
    }
    else
    {
        OSL_FAIL("shape was not in internal list");
    }
}

bool ScChildrenShapes::FindShape(const uno::Reference<drawing::XShape>& xShape, ScChildrenShapes::SortedShapes::iterator& rItr) const
{
    if (mbShapesNeedSorting)
    {
        std::sort(maZOrderedShapes.begin(), maZOrderedShapes.end(), ScShapeDataLess());
        mbShapesNeedSorting = false;
    }
    bool bResult(false);
    ScAccessibleShapeData aShape(xShape);
    rItr = std::lower_bound(maZOrderedShapes.begin(), maZOrderedShapes.end(), &aShape, ScShapeDataLess());
    if ((rItr != maZOrderedShapes.end()) && (*rItr != nullptr) && ((*rItr)->xShape.get() == xShape.get()))
        bResult = true; // if the shape is found

#if OSL_DEBUG_LEVEL > 0 // test whether it finds truly the correct shape (perhaps it is not really sorted)
    SortedShapes::iterator aDebugItr = std::find_if(maZOrderedShapes.begin(), maZOrderedShapes.end(),
        [&xShape](const ScAccessibleShapeData* pShape) { return pShape && (pShape->xShape.get() == xShape.get()); });
    bool bResult2 = (aDebugItr != maZOrderedShapes.end());
    OSL_ENSURE((bResult == bResult2) && ((bResult && (rItr == aDebugItr)) || !bResult), "wrong Shape found");
#endif
    return bResult;
}

sal_Int8 ScChildrenShapes::Compare(const ScAccessibleShapeData* pData1,
        const ScAccessibleShapeData* pData2)
{
    ScShapeDataLess aLess;

    bool bResult1(aLess(pData1, pData2));
    bool bResult2(aLess(pData2, pData1));

    sal_Int8 nResult(0);
    if (!bResult1 && bResult2)
        nResult = 1;
    else if (bResult1 && !bResult2)
        nResult = -1;

    return nResult;
}

void ScChildrenShapes::VisAreaChanged() const
{
    for (const ScAccessibleShapeData* pAccShapeData: maZOrderedShapes)
        if (pAccShapeData && pAccShapeData->pAccShape.is())
            pAccShapeData->pAccShape->ViewForwarderChanged();
}

ScAccessibleDocument::ScAccessibleDocument(
        const uno::Reference<XAccessible>& rxParent,
        ScTabViewShell* pViewShell,
        ScSplitPos eSplitPos)
    : ImplInheritanceHelper(rxParent),
    mpViewShell(pViewShell),
    meSplitPos(eSplitPos),
    mbCompleteSheetSelected(false)
{
    maVisArea = GetVisibleArea_Impl();
}

void ScAccessibleDocument::PreInit()
{
    if (!mpViewShell)
        return;

    mpViewShell->AddAccessibilityObject(*this);
    vcl::Window *pWin = mpViewShell->GetWindowByPos(meSplitPos);
    if( pWin )
    {
        pWin->AddChildEventListener( LINK( this, ScAccessibleDocument, WindowChildEventListener ));
        sal_uInt16 nCount =   pWin->GetChildCount();
        for( sal_uInt16 i=0; i < nCount; ++i )
        {
            vcl::Window *pChildWin = pWin->GetChild( i );
            if( pChildWin &&
                AccessibleRole::EMBEDDED_OBJECT == pChildWin->GetAccessibleRole() )
                AddChild( pChildWin->GetAccessible(), false );
        }
    }
    ScViewData& rViewData = mpViewShell->GetViewData();
    if (rViewData.HasEditView(meSplitPos))
    {
        uno::Reference<XAccessible> xAcc = new ScAccessibleEditObject(this, rViewData.GetEditView(meSplitPos),
            mpViewShell->GetWindowByPos(meSplitPos), GetCurrentCellName(), GetCurrentCellDescription(),
            ScAccessibleEditObject::CellInEditMode);
        AddChild(xAcc, false);
    }
}

void ScAccessibleDocument::Init()
{
    if(!mpChildrenShapes)
        mpChildrenShapes.reset( new ScChildrenShapes(this, mpViewShell, meSplitPos) );
}

ScAccessibleDocument::~ScAccessibleDocument()
{
    if (!ScAccessibleContextBase::IsDefunc() && !rBHelper.bInDispose)
    {
        // increment refcount to prevent double call of dtor
        osl_atomic_increment( &m_refCount );
        dispose();
    }
}

void SAL_CALL ScAccessibleDocument::disposing()
{
    SolarMutexGuard aGuard;
    FreeAccessibleSpreadsheet();
    if (mpViewShell)
    {
        vcl::Window *pWin = mpViewShell->GetWindowByPos(meSplitPos);
        if( pWin )
            pWin->RemoveChildEventListener( LINK( this, ScAccessibleDocument, WindowChildEventListener ));

        mpViewShell->RemoveAccessibilityObject(*this);
        mpViewShell = nullptr;
    }
    mpChildrenShapes.reset();

    ScAccessibleDocumentBase::disposing();
}

void SAL_CALL ScAccessibleDocument::disposing( const lang::EventObject& /* Source */ )
{
    disposing();
}

    //=====  SfxListener  =====================================================

IMPL_LINK( ScAccessibleDocument, WindowChildEventListener, VclWindowEvent&, rEvent, void )
{
    OSL_ENSURE( rEvent.GetWindow(), "Window???" );
    switch ( rEvent.GetId() )
    {
    case VclEventId::WindowShow:  // send create on show for direct accessible children
        {
            vcl::Window* pChildWin = static_cast < vcl::Window * >( rEvent.GetData() );
            if( pChildWin && AccessibleRole::EMBEDDED_OBJECT == pChildWin->GetAccessibleRole() )
            {
                AddChild( pChildWin->GetAccessible(), true );
            }
        }
        break;
    case VclEventId::WindowHide:  // send destroy on hide for direct accessible children
        {
            vcl::Window* pChildWin = static_cast < vcl::Window * >( rEvent.GetData() );
            if( pChildWin && AccessibleRole::EMBEDDED_OBJECT == pChildWin->GetAccessibleRole() )
            {
                RemoveChild( pChildWin->GetAccessible(), true );
            }
        }
        break;
    default: break;
    }
}

void ScAccessibleDocument::Notify( SfxBroadcaster& rBC, const SfxHint& rHint )
{
    if (rHint.GetId() == SfxHintId::ScAccGridWinFocusLost )
    {
        auto pFocusLostHint = static_cast<const ScAccGridWinFocusLostHint*>(&rHint);
        if (pFocusLostHint->GetOldGridWin() == meSplitPos)
        {
            if (mxTempAcc.is() && mpTempAccEdit)
                mpTempAccEdit->LostFocus();
            else if (mpAccessibleSpreadsheet.is())
                mpAccessibleSpreadsheet->LostFocus();
            else
                CommitFocusLost();
        }
    }
    else if (rHint.GetId() == SfxHintId::ScAccGridWinFocusGot)
    {
        auto pFocusGotHint = static_cast<const ScAccGridWinFocusGotHint*>(&rHint);
        if (pFocusGotHint->GetNewGridWin() == meSplitPos)
        {
            rtl::Reference<::accessibility::AccessibleShape> xAccShape;
            if (mpChildrenShapes)
            {
                bool bTabMarked(IsTableSelected());
                xAccShape = mpChildrenShapes->GetSelected(0, bTabMarked);
            }
            if (xAccShape.is())
            {
                uno::Any aNewValue;
                aNewValue<<=AccessibleStateType::FOCUSED;
                xAccShape->CommitChange(AccessibleEventId::STATE_CHANGED, aNewValue, uno::Any(),
                                        -1);
            }
            else
            {
            if (mxTempAcc.is() && mpTempAccEdit)
                mpTempAccEdit->GotFocus();
            else if (mpAccessibleSpreadsheet.is())
                mpAccessibleSpreadsheet->GotFocus();
            else
                CommitFocusGained();
            }
        }
    }
    else if (rHint.GetId() == SfxHintId::ScAccTableChanged)
    {
        // only notify if child exist, otherwise it is not necessary
        if (mpAccessibleSpreadsheet.is())
        {
            FreeAccessibleSpreadsheet();

            // Shapes / form controls after reload not accessible, rebuild the
            // mpChildrenShapes variable.
            mpChildrenShapes.reset( new ScChildrenShapes( this, mpViewShell, meSplitPos ) );

            // all children changed
            CommitChange(AccessibleEventId::INVALIDATE_ALL_CHILDREN, uno::Any(), uno::Any());

            if (mpAccessibleSpreadsheet.is())
                mpAccessibleSpreadsheet->GotFocus();
        }
    }
    else if (rHint.GetId() == SfxHintId::ScAccMakeDrawLayer)
    {
        if (mpChildrenShapes)
            mpChildrenShapes->SetDrawBroadcaster();
    }
    else if (rHint.GetId() == SfxHintId::ScAccEnterEditMode) // this event comes only on creating edit field of a cell
    {
        if (mpViewShell->GetViewData().GetEditActivePart() == meSplitPos)
        {
            ScViewData& rViewData = mpViewShell->GetViewData();
            EditEngine const& rEditEng = rViewData.GetEditView(meSplitPos)->getEditEngine();
            if (rEditEng.IsUpdateLayout())
            {
                mpTempAccEdit = new ScAccessibleEditObject(this, rViewData.GetEditView(meSplitPos),
                    mpViewShell->GetWindowByPos(meSplitPos), GetCurrentCellName(),
                    ScResId(STR_ACC_EDITLINE_DESCR), ScAccessibleEditObject::CellInEditMode);

                AddChild(uno::Reference<XAccessible>(mpTempAccEdit), true);

                if (mpAccessibleSpreadsheet.is())
                    mpAccessibleSpreadsheet->LostFocus();
                else
                    CommitFocusLost();

                mpTempAccEdit->GotFocus();
            }
        }
    }
    else if (rHint.GetId() == SfxHintId::ScAccLeaveEditMode)
    {
        if (mxTempAcc.is())
        {
            if (mpTempAccEdit)
            {
                mpTempAccEdit->LostFocus();
            }
            RemoveChild(mxTempAcc, true);
            if (mpTempAccEdit)
            {
                // tdf#125982 a11y use-after-free of editengine by
                // ScAccessibleEditObjectTextData living past the
                // the editengine of the editview passed in above
                // in ScAccEnterEditMode
                mpTempAccEdit->dispose();
                mpTempAccEdit = nullptr;
            }
            if (mpAccessibleSpreadsheet.is() && mpViewShell && mpViewShell->IsActive())
                mpAccessibleSpreadsheet->GotFocus();
            else if( mpViewShell && mpViewShell->IsActive())
                CommitFocusGained();
        }
    }
    else if ((rHint.GetId() == SfxHintId::ScAccVisAreaChanged) || (rHint.GetId() == SfxHintId::ScAccWindowResized))
    {
        tools::Rectangle aOldVisArea(maVisArea);
        maVisArea = GetVisibleArea_Impl();

        if (maVisArea != aOldVisArea)
        {
            if (maVisArea.GetSize() != aOldVisArea.GetSize())
            {
                CommitChange(AccessibleEventId::BOUNDRECT_CHANGED, uno::Any(), uno::Any());

                if (mpAccessibleSpreadsheet.is())
                    mpAccessibleSpreadsheet->BoundingBoxChanged();
                if (mpAccessibleSpreadsheet.is() && mpViewShell && mpViewShell->IsActive())
                    mpAccessibleSpreadsheet->FireFirstCellFocus();
            }
            else if (mpAccessibleSpreadsheet.is())
            {
                mpAccessibleSpreadsheet->VisAreaChanged();
            }
            if (mpChildrenShapes)
                mpChildrenShapes->VisAreaChanged();
        }
    }

    ScAccessibleDocumentBase::Notify(rBC, rHint);
}

void SAL_CALL ScAccessibleDocument::selectionChanged( const lang::EventObject& /* aEvent */ )
{
    bool bSelectionChanged(false);
    if (mpAccessibleSpreadsheet.is())
    {
        bool bOldSelected(mbCompleteSheetSelected);
        mbCompleteSheetSelected = IsTableSelected();
        if (bOldSelected != mbCompleteSheetSelected)
        {
            mpAccessibleSpreadsheet->CompleteSelectionChanged(mbCompleteSheetSelected);
            bSelectionChanged = true;
        }
    }

    if (mpChildrenShapes && mpChildrenShapes->SelectionChanged())
        bSelectionChanged = true;

    if (bSelectionChanged)
        CommitChange(AccessibleEventId::SELECTION_CHANGED, uno::Any(), uno::Any());
}

    //=====  XAccessibleComponent  ============================================

uno::Reference< XAccessible > SAL_CALL ScAccessibleDocument::getAccessibleAtPoint(
        const awt::Point& rPoint )
{
    uno::Reference<XAccessible> xAccessible;
    if (containsPoint(rPoint))
    {
        SolarMutexGuard aGuard;
        ensureAlive();
        if (mpChildrenShapes)
            xAccessible = mpChildrenShapes->GetAt(rPoint);
        if(!xAccessible.is())
        {
            if (mxTempAcc.is())
            {
                uno::Reference< XAccessibleContext > xCont(mxTempAcc->getAccessibleContext());
                uno::Reference< XAccessibleComponent > xComp(xCont, uno::UNO_QUERY);
                if (xComp.is())
                {
                    tools::Rectangle aBound(vcl::unohelper::ConvertToVCLRect(xComp->getBounds()));
                    if (aBound.Contains(vcl::unohelper::ConvertToVCLPoint(rPoint)))
                        xAccessible = mxTempAcc;
                }
            }
            if (!xAccessible.is())
                xAccessible = GetAccessibleSpreadsheet();
        }
    }
    return xAccessible;
}

void SAL_CALL ScAccessibleDocument::grabFocus(  )
{
    SolarMutexGuard aGuard;
    ensureAlive();
    if (!getAccessibleParent().is())
        return;

    uno::Reference<XAccessibleComponent> xAccessibleComponent(getAccessibleParent()->getAccessibleContext(), uno::UNO_QUERY);
    if (xAccessibleComponent.is())
    {
        xAccessibleComponent->grabFocus();
        // grab only focus if it does not have the focus and it is not hidden
        if (mpViewShell &&
            (mpViewShell->GetViewData().GetActivePart() != meSplitPos) &&
            mpViewShell->GetWindowByPos(meSplitPos)->IsVisible())
        {
            mpViewShell->ActivatePart(meSplitPos);
        }
    }
}

    //=====  XAccessibleContext  ==============================================

    /// Return the number of currently visible children.
sal_Int64 SAL_CALL
    ScAccessibleDocument::getAccessibleChildCount()
{
    SolarMutexGuard aGuard;
    ensureAlive();
    sal_Int64 nCount(1);
    if (mpChildrenShapes)
        nCount = mpChildrenShapes->GetCount(); // returns the count of the shapes inclusive the table

    if (mxTempAcc.is())
        ++nCount;

    return nCount;
}

    /// Return the specified child or NULL if index is invalid.
uno::Reference<XAccessible> SAL_CALL
    ScAccessibleDocument::getAccessibleChild(sal_Int64 nIndex)
{
    SolarMutexGuard aGuard;
    ensureAlive();
    uno::Reference<XAccessible> xAccessible;
    if (nIndex >= 0)
    {
        sal_Int64 nCount(1);
        if (mpChildrenShapes)
        {
            xAccessible = mpChildrenShapes->Get(nIndex); // returns NULL if it is the table or out of range
            nCount = mpChildrenShapes->GetCount(); //there is always a table
        }
        if (!xAccessible.is())
        {
            if (nIndex < nCount)
                xAccessible = GetAccessibleSpreadsheet();
            else if (nIndex == nCount && mxTempAcc.is())
                xAccessible = mxTempAcc;
        }
    }

    if (!xAccessible.is())
        throw lang::IndexOutOfBoundsException();

    return xAccessible;
}

    /// Return the set of current states.
sal_Int64 SAL_CALL
    ScAccessibleDocument::getAccessibleStateSet()
{
    SolarMutexGuard aGuard;
    sal_Int64 nParentStates = 0;
    if (getAccessibleParent().is())
    {
        uno::Reference<XAccessibleContext> xParentContext = getAccessibleParent()->getAccessibleContext();
        nParentStates = xParentContext->getAccessibleStateSet();
    }
    sal_Int64 nStateSet = 0;
    if (IsDefunc(nParentStates))
        nStateSet |= AccessibleStateType::DEFUNC;
    else
    {
        nStateSet |= AccessibleStateType::EDITABLE;
        nStateSet |= AccessibleStateType::ENABLED;
        nStateSet |= AccessibleStateType::OPAQUE;
        if (isShowing())
            nStateSet |= AccessibleStateType::SHOWING;
        if (isVisible())
            nStateSet |= AccessibleStateType::VISIBLE;
    }
    return nStateSet;
}

OUString SAL_CALL
    ScAccessibleDocument::getAccessibleName()
{
    SolarMutexGuard g;

    OUString aName = ScResId(STR_ACC_DOC_SPREADSHEET);
    ScDocument* pScDoc = GetDocument();
    if (!pScDoc)
        return aName;

    ScDocShell* pObjSh = pScDoc->GetDocumentShell();
    if (!pObjSh)
        return aName;

    OUString aFileName;
    SfxMedium* pMed = pObjSh->GetMedium();
    if (pMed)
        aFileName = pMed->GetName();

    if (aFileName.isEmpty())
        aFileName = pObjSh->GetTitle(SFX_TITLE_APINAME);

    if (!aFileName.isEmpty())
    {
        OUString aReadOnly;
        if (pObjSh->IsReadOnly())
            aReadOnly = ScResId(STR_ACC_DOC_SPREADSHEET_READONLY);

        aName = aFileName + aReadOnly + " - " + aName;
    }
    return aName;
}

///=====  XAccessibleSelection  ===========================================

void SAL_CALL
    ScAccessibleDocument::selectAccessibleChild( sal_Int64 nChildIndex )
{
    SolarMutexGuard aGuard;
    ensureAlive();

    if (!(mpChildrenShapes && mpViewShell))
        return;

    sal_Int32 nCount(mpChildrenShapes->GetCount()); // all shapes and the table
    if (mxTempAcc.is())
        ++nCount;
    if (nChildIndex < 0 || nChildIndex >= nCount)
        throw lang::IndexOutOfBoundsException();

    uno::Reference < XAccessible > xAccessible = mpChildrenShapes->Get(nChildIndex);
    if (xAccessible.is())
    {
        bool bWasTableSelected(IsTableSelected());
        mpChildrenShapes->Select(nChildIndex); // throws no lang::IndexOutOfBoundsException if Index is too high
        if (bWasTableSelected)
            mpViewShell->SelectAll();
    }
    else
    {
        mpViewShell->SelectAll();
    }
}

sal_Bool SAL_CALL
    ScAccessibleDocument::isAccessibleChildSelected( sal_Int64 nChildIndex )
{
    SolarMutexGuard aGuard;
    ensureAlive();
    bool bResult(false);

    if (mpChildrenShapes)
    {
        sal_Int32 nCount(mpChildrenShapes->GetCount()); // all shapes and the table
        if (mxTempAcc.is())
            ++nCount;
        if (nChildIndex < 0 || nChildIndex >= nCount)
            throw lang::IndexOutOfBoundsException();

        uno::Reference < XAccessible > xAccessible = mpChildrenShapes->Get(nChildIndex);
        if (xAccessible.is())
        {
            uno::Reference<drawing::XShape> xShape;
            bResult = mpChildrenShapes->IsSelected(nChildIndex, xShape); // throws no lang::IndexOutOfBoundsException if Index is too high
        }
        else
        {
            if (mxTempAcc.is() && nChildIndex == nCount)
                bResult = true;
            else
                bResult = IsTableSelected();
        }
    }
    return bResult;
}

void SAL_CALL
    ScAccessibleDocument::clearAccessibleSelection(  )
{
    SolarMutexGuard aGuard;
    ensureAlive();

    if (mpChildrenShapes)
        mpChildrenShapes->DeselectAll(); //deselects all (also the table)
}

void SAL_CALL
    ScAccessibleDocument::selectAllAccessibleChildren(  )
{
    SolarMutexGuard aGuard;
    ensureAlive();

    if (mpChildrenShapes)
        mpChildrenShapes->SelectAll();

    // select table after shapes, because while selecting shapes the table will be deselected
    if (mpViewShell)
    {
        mpViewShell->SelectAll();
    }
}

sal_Int64 SAL_CALL
    ScAccessibleDocument::getSelectedAccessibleChildCount(  )
{
    SolarMutexGuard aGuard;
    ensureAlive();
    sal_Int64 nCount(0);

    if (mpChildrenShapes)
        nCount = mpChildrenShapes->GetSelectedCount();

    if (IsTableSelected())
        ++nCount;

    if (mxTempAcc.is())
        ++nCount;

    return nCount;
}

uno::Reference<XAccessible > SAL_CALL
    ScAccessibleDocument::getSelectedAccessibleChild( sal_Int64 nSelectedChildIndex )
{
    SolarMutexGuard aGuard;
    ensureAlive();
    uno::Reference<XAccessible> xAccessible;
    if (mpChildrenShapes)
    {
        sal_Int64 nCount(getSelectedAccessibleChildCount()); //all shapes and the table
        if (nSelectedChildIndex < 0 || nSelectedChildIndex >= nCount)
            throw lang::IndexOutOfBoundsException();

        bool bTabMarked(IsTableSelected());

        if (mpChildrenShapes)
            xAccessible = mpChildrenShapes->GetSelected(nSelectedChildIndex, bTabMarked); // throws no lang::IndexOutOfBoundsException if Index is too high
        if (mxTempAcc.is() && nSelectedChildIndex == nCount - 1)
            xAccessible = mxTempAcc;
        else if (bTabMarked)
            xAccessible = GetAccessibleSpreadsheet();
    }

    OSL_ENSURE(xAccessible.is(), "here should always be an accessible object or an exception thrown");

    return xAccessible;
}

void SAL_CALL
    ScAccessibleDocument::deselectAccessibleChild( sal_Int64 nChildIndex )
{
    SolarMutexGuard aGuard;
    ensureAlive();

    if (!(mpChildrenShapes && mpViewShell))
        return;

    sal_Int32 nCount(mpChildrenShapes->GetCount()); // all shapes and the table
    if (mxTempAcc.is())
        ++nCount;
    if (nChildIndex < 0 || nChildIndex >= nCount)
        throw lang::IndexOutOfBoundsException();

    bool bTabMarked(IsTableSelected());

    uno::Reference < XAccessible > xAccessible = mpChildrenShapes->Get(nChildIndex);
    if (xAccessible.is())
    {
        mpChildrenShapes->Deselect(nChildIndex); // throws no lang::IndexOutOfBoundsException if Index is too high
        if (bTabMarked)
            mpViewShell->SelectAll(); // select the table again
    }
    else if (bTabMarked)
        mpViewShell->Unmark();
}

    //=====  XServiceInfo  ====================================================

OUString SAL_CALL
    ScAccessibleDocument::getImplementationName()
{
    return u"ScAccessibleDocument"_ustr;
}

///=====  IAccessibleViewForwarder  ========================================

tools::Rectangle ScAccessibleDocument::GetVisibleArea_Impl()
{
    tools::Rectangle aVisRect(GetBoundingBox());

    if (mpViewShell)
    {
        Point aPoint(mpViewShell->GetViewData().GetPixPos(meSplitPos)); // returns a negative Point
        aPoint.setX(-aPoint.getX());
        aPoint.setY(-aPoint.getY());
        aVisRect.SetPos(aPoint);

        ScGridWindow* pWin = static_cast<ScGridWindow*>(mpViewShell->GetWindowByPos(meSplitPos));
        if (pWin)
            aVisRect = pWin->PixelToLogic(aVisRect, pWin->GetDrawMapMode());
    }

    return aVisRect;
}

tools::Rectangle ScAccessibleDocument::GetVisibleArea() const
{
    SolarMutexGuard aGuard;
    ensureAlive();
    return maVisArea;
}

Point ScAccessibleDocument::LogicToPixel (const Point& rPoint) const
{
    SolarMutexGuard aGuard;
    ensureAlive();
    Point aPoint;
    ScGridWindow* pWin = static_cast<ScGridWindow*>(mpViewShell->GetWindowByPos(meSplitPos));
    if (pWin)
    {
        aPoint = pWin->LogicToPixel(rPoint, pWin->GetDrawMapMode());
        aPoint += Point(pWin->GetWindowExtentsAbsolute().TopLeft());
    }
    return aPoint;
}

Size ScAccessibleDocument::LogicToPixel (const Size& rSize) const
{
    SolarMutexGuard aGuard;
    ensureAlive();
    Size aSize;
    ScGridWindow* pWin = static_cast<ScGridWindow*>(mpViewShell->GetWindowByPos(meSplitPos));
    if (pWin)
        aSize = pWin->LogicToPixel(rSize, pWin->GetDrawMapMode());
    return aSize;
}

rtl::Reference<utl::AccessibleRelationSetHelper> ScAccessibleDocument::GetRelationSet(const ScAddress* pAddress) const
{
    rtl::Reference<utl::AccessibleRelationSetHelper> pRelationSet;
    if (mpChildrenShapes)
        pRelationSet = mpChildrenShapes->GetRelationSet(pAddress);
    return pRelationSet;
}

OUString
    ScAccessibleDocument::createAccessibleDescription()
{
    return STR_ACC_DOC_DESCR;
}

OUString
    ScAccessibleDocument::createAccessibleName()
{
    SolarMutexGuard aGuard;
    ensureAlive();
    OUString sName = ScResId(STR_ACC_DOC_NAME);
    sal_Int32 nNumber(sal_Int32(meSplitPos) + 1);
    sName += OUString::number(nNumber);
    return sName;
}

AbsoluteScreenPixelRectangle ScAccessibleDocument::GetBoundingBoxOnScreen()
{
    AbsoluteScreenPixelRectangle aRect;
    if (mpViewShell)
    {
        vcl::Window* pWindow = mpViewShell->GetWindowByPos(meSplitPos);
        if (pWindow)
            aRect = pWindow->GetWindowExtentsAbsolute();
    }
    return aRect;
}

tools::Rectangle ScAccessibleDocument::GetBoundingBox()
{
    tools::Rectangle aRect;
    if (mpViewShell)
    {
        vcl::Window* pWindow = mpViewShell->GetWindowByPos(meSplitPos);
        if (pWindow)
            aRect = pWindow->GetWindowExtentsRelative(*pWindow->GetAccessibleParentWindow());
    }
    return aRect;
}

SCTAB ScAccessibleDocument::getVisibleTable() const
{
    SCTAB nVisibleTable(0);
    if (mpViewShell)
        nVisibleTable = mpViewShell->GetViewData().GetTabNo();
    return nVisibleTable;
}

uno::Reference < XAccessible >
    ScAccessibleDocument::GetAccessibleSpreadsheet()
{
    if (!mpAccessibleSpreadsheet.is() && mpViewShell)
    {
        mpAccessibleSpreadsheet = new ScAccessibleSpreadsheet(this, mpViewShell, getVisibleTable(), meSplitPos);
        mpAccessibleSpreadsheet->Init();
        mbCompleteSheetSelected = IsTableSelected();
    }
    return mpAccessibleSpreadsheet;
}

void ScAccessibleDocument::FreeAccessibleSpreadsheet()
{
    if (mpAccessibleSpreadsheet.is())
    {
        mpAccessibleSpreadsheet->dispose();
        mpAccessibleSpreadsheet.clear();
    }
}

bool ScAccessibleDocument::IsTableSelected() const
{
    bool bResult (false);
    if(mpViewShell)
    {
        SCTAB nTab(getVisibleTable());
        //#103800#; use a copy of MarkData
        ScMarkData aMarkData(mpViewShell->GetViewData().GetMarkData());
        ScDocument* pDoc = GetDocument();
        if (aMarkData.IsAllMarked( ScRange( 0, 0, nTab, pDoc->MaxCol(), pDoc->MaxRow(), nTab)))
            bResult = true;
    }
    return bResult;
}

bool ScAccessibleDocument::IsDefunc(sal_Int64 nParentStates)
{
    return ScAccessibleContextBase::IsDefunc() || (mpViewShell == nullptr) || !getAccessibleParent().is() ||
        (nParentStates & AccessibleStateType::DEFUNC);
}

void ScAccessibleDocument::AddChild(const uno::Reference<XAccessible>& xAcc, bool bFireEvent)
{
    OSL_ENSURE(!mxTempAcc.is(), "this object should be removed before");
    if (xAcc.is())
    {
        mxTempAcc = xAcc;
        if( bFireEvent )
        {
            CommitChange(AccessibleEventId::CHILD, uno::Any(), uno::Any(mxTempAcc),
                         getAccessibleChildCount() - 1);
        }
    }
}

void ScAccessibleDocument::RemoveChild(const uno::Reference<XAccessible>& xAcc, bool bFireEvent)
{
    OSL_ENSURE(mxTempAcc.is(), "this object should be added before");
    if (!xAcc.is())
        return;

    OSL_ENSURE(xAcc.get() == mxTempAcc.get(), "only the same object should be removed");
    if( bFireEvent )
        CommitChange(AccessibleEventId::CHILD, uno::Any(mxTempAcc), uno::Any());
    mxTempAcc = nullptr;
}

OUString ScAccessibleDocument::GetCurrentCellName() const
{
    OUString sName(ScResId(STR_ACC_CELL_NAME));
    if (mpViewShell)
    {
        // Document not needed, because only the cell address, but not the tablename is needed
        OUString sAddress(mpViewShell->GetViewData().GetCurPos().Format(ScRefFlags::VALID));
        sName = sName.replaceFirst("%1", sAddress);
    }
    return sName;
}

const OUString & ScAccessibleDocument::GetCurrentCellDescription()
{
    return EMPTY_OUSTRING;
}

ScDocument *ScAccessibleDocument::GetDocument() const
{
    return mpViewShell ? &mpViewShell->GetViewData().GetDocument() : nullptr;
}

ScAddress   ScAccessibleDocument::GetCurCellAddress() const
{
    return mpViewShell ? mpViewShell->GetViewData().GetCurPos() : ScAddress();
}

OUString SAL_CALL ScAccessibleDocument::getExtendedAttributes()
{
    SolarMutexGuard g;

    sal_uInt16 sheetIndex;
    OUString sSheetName;
    sheetIndex = getVisibleTable();
    if(GetDocument()==nullptr)
        return OUString();
    GetDocument()->GetName(sheetIndex,sSheetName);
    OUString sValue = "page-name:" + sSheetName +
        ";page-number:" + OUString::number(sheetIndex+1) +
        ";total-pages:" + OUString::number(GetDocument()->GetTableCount()) + ";";
    return sValue;
}

sal_Int32 SAL_CALL ScAccessibleDocument::getForeground(  )
{
    return sal_Int32(COL_BLACK);
}

sal_Int32 SAL_CALL ScAccessibleDocument::getBackground(  )
{
    SolarMutexGuard aGuard;
    ensureAlive();
    return sal_Int32(ScModule::get()->GetColorConfig().GetColorValue(::svtools::DOCCOLOR).nColor);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

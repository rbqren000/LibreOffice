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

#include "PresenterViewFactory.hxx"
#include "PresenterPaneContainer.hxx"
#include "PresenterHelpView.hxx"
#include "PresenterNotesView.hxx"
#include "PresenterSlideShowView.hxx"
#include "PresenterSlidePreview.hxx"
#include "PresenterSlideSorter.hxx"
#include "PresenterToolBar.hxx"
#include <DrawController.hxx>
#include <framework/ConfigurationController.hxx>
#include <utility>

using namespace ::com::sun::star;
using namespace ::com::sun::star::uno;
using namespace ::com::sun::star::drawing::framework;

namespace sdext::presenter {

namespace {

/** By default the PresenterSlidePreview shows the preview of the current
    slide.  This adapter class makes it display the preview of the next
    slide.
*/
class NextSlidePreview : public PresenterSlidePreview
{
public:
    NextSlidePreview (
        const css::uno::Reference<css::uno::XComponentContext>& rxContext,
        const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId,
        const css::uno::Reference<css::drawing::framework::XPane>& rxAnchorPane,
        const ::rtl::Reference<PresenterController>& rpPresenterController)
        : PresenterSlidePreview(rxContext, rxViewId, rxAnchorPane, rpPresenterController)
    {
    }

    virtual void SAL_CALL setCurrentPage (
        const css::uno::Reference<css::drawing::XDrawPage>& rxSlide) override
    {
        Reference<presentation::XSlideShowController> xSlideShowController (
            mpPresenterController->GetSlideShowController());
        Reference<drawing::XDrawPage> xSlide;
        if (xSlideShowController.is())
        {
            const sal_Int32 nCount (xSlideShowController->getSlideCount());
            sal_Int32 nNextSlideIndex (-1);
            if (xSlideShowController->getCurrentSlide() == rxSlide)
            {
                nNextSlideIndex = xSlideShowController->getNextSlideIndex();
            }
            else
            {
                for (sal_Int32 nIndex=0; nIndex<nCount; ++nIndex)
                {
                    if (rxSlide == xSlideShowController->getSlideByIndex(nIndex))
                    {
                        nNextSlideIndex = nIndex + 1;
                    }
                }
            }
            if (nNextSlideIndex >= 0)
            {
                if (nNextSlideIndex < nCount)
                {
                    xSlide = xSlideShowController->getSlideByIndex(nNextSlideIndex);
                }
            }
        }
        PresenterSlidePreview::setCurrentPage(xSlide);
    }
};

} // end of anonymous namespace

//===== PresenterViewFactory ==============================================

PresenterViewFactory::PresenterViewFactory (
    const Reference<uno::XComponentContext>& rxContext,
    const rtl::Reference<::sd::DrawController>& rxController,
    ::rtl::Reference<PresenterController> pPresenterController)
    : mxComponentContext(rxContext),
      mxControllerWeak(rxController),
      mpPresenterController(std::move(pPresenterController))
{
}

rtl::Reference<sd::framework::ResourceFactory> PresenterViewFactory::Create (
    const Reference<uno::XComponentContext>& rxContext,
    const rtl::Reference<::sd::DrawController>& rxController,
    const ::rtl::Reference<PresenterController>& rpPresenterController)
{
    rtl::Reference<PresenterViewFactory> pFactory (
        new PresenterViewFactory(rxContext,rxController,rpPresenterController));
    pFactory->Register(rxController);
    return pFactory;
}

void PresenterViewFactory::Register (const rtl::Reference<::sd::DrawController>& rxController)
{
    try
    {
        // Get the configuration controller.
        mxConfigurationController = rxController->getConfigurationController();
        if ( ! mxConfigurationController.is())
        {
            throw RuntimeException();
        }
        mxConfigurationController->addResourceFactory(msCurrentSlidePreviewViewURL, this);
        mxConfigurationController->addResourceFactory(msNextSlidePreviewViewURL, this);
        mxConfigurationController->addResourceFactory(msNotesViewURL, this);
        mxConfigurationController->addResourceFactory(msToolBarViewURL, this);
        mxConfigurationController->addResourceFactory(msSlideSorterURL, this);
        mxConfigurationController->addResourceFactory(msHelpViewURL, this);
    }
    catch (RuntimeException&)
    {
        OSL_ASSERT(false);
        if (mxConfigurationController.is())
            mxConfigurationController->removeResourceFactoryForReference(this);
        mxConfigurationController = nullptr;

        throw;
    }
}

PresenterViewFactory::~PresenterViewFactory()
{
}

void PresenterViewFactory::disposing(std::unique_lock<std::mutex>&)
{
    if (mxConfigurationController.is())
        mxConfigurationController->removeResourceFactoryForReference(this);
    mxConfigurationController = nullptr;

    if (mpResourceCache == nullptr)
        return;

    // Dispose all views in the cache.
    for (const auto& rView : *mpResourceCache)
    {
        try
        {
            Reference<lang::XComponent> xComponent (rView.second.first, UNO_QUERY);
            if (xComponent.is())
                xComponent->dispose();
        }
        catch (lang::DisposedException&)
        {
        }
    }
    mpResourceCache.reset();
}

//----- XViewFactory ----------------------------------------------------------

Reference<XResource> PresenterViewFactory::createResource (
    const Reference<XResourceId>& rxViewId)
{
    {
        std::unique_lock l(m_aMutex);
        throwIfDisposed(l);
    }

    Reference<XResource> xView;

    if (rxViewId.is())
    {
        Reference<XPane> xAnchorPane (
            mxConfigurationController->getResource(rxViewId->getAnchor()),
            UNO_QUERY_THROW);
        xView = GetViewFromCache(rxViewId, xAnchorPane);
        if (xView == nullptr)
            xView = CreateView(rxViewId, xAnchorPane);

        // Activate the view.
        PresenterPaneContainer::SharedPaneDescriptor pDescriptor (
            mpPresenterController->GetPaneContainer()->FindPaneId(rxViewId->getAnchor()));
        if (pDescriptor)
            pDescriptor->SetActivationState(true);
    }

    return xView;
}

void PresenterViewFactory::releaseResource (const Reference<XResource>& rxView)
{
    {
        std::unique_lock l(m_aMutex);
        throwIfDisposed(l);
    }

    if ( ! rxView.is())
        return;

    // Deactivate the view.
    PresenterPaneContainer::SharedPaneDescriptor pDescriptor (
        mpPresenterController->GetPaneContainer()->FindPaneId(
            rxView->getResourceId()->getAnchor()));
    if (pDescriptor)
        pDescriptor->SetActivationState(false);

    // Dispose only views that we can not put into the cache.
    CachablePresenterView* pView = dynamic_cast<CachablePresenterView*>(rxView.get());
    if (pView == nullptr || mpResourceCache == nullptr)
    {
        try
        {
            if (pView != nullptr)
                pView->ReleaseView();
            Reference<lang::XComponent> xComponent (rxView, UNO_QUERY);
            if (xComponent.is())
                xComponent->dispose();
        }
        catch (lang::DisposedException&)
        {
            // Do not let disposed exceptions get out.  It might be interpreted
            // as coming from the factory, which would then be removed from the
            // drawing framework.
        }
    }
    else
    {
        // Put cacheable views in the cache.
        Reference<XResourceId> xViewId (rxView->getResourceId());
        if (xViewId.is())
        {
            Reference<XPane> xAnchorPane (
                mxConfigurationController->getResource(xViewId->getAnchor()),
                UNO_QUERY_THROW);
            (*mpResourceCache)[xViewId->getResourceURL()]
                = ViewResourceDescriptor(Reference<XView>(rxView, UNO_QUERY), xAnchorPane);
            pView->DeactivatePresenterView();
        }
    }
}


Reference<XResource> PresenterViewFactory::GetViewFromCache(
    const Reference<XResourceId>& rxViewId,
    const Reference<XPane>& rxAnchorPane) const
{
    if (mpResourceCache == nullptr)
        return nullptr;

    try
    {
        const OUString sResourceURL (rxViewId->getResourceURL());

        // Can we use a view from the cache?
        ResourceContainer::const_iterator iView (mpResourceCache->find(sResourceURL));
        if (iView != mpResourceCache->end())
        {
            // The view is in the container but it can only be used if
            // the anchor pane is the same now as it was at creation of
            // the view.
            if (iView->second.second == rxAnchorPane)
            {
                CachablePresenterView* pView
                    = dynamic_cast<CachablePresenterView*>(iView->second.first.get());
                if (pView != nullptr)
                    pView->ActivatePresenterView();
                return iView->second.first;
            }

            // Right view, wrong pane.  Create a new view.
        }
    }
    catch (RuntimeException&)
    {
    }
    return nullptr;
}

Reference<XResource> PresenterViewFactory::CreateView(
    const Reference<XResourceId>& rxViewId,
    const Reference<XPane>& rxAnchorPane)
{
    Reference<XView> xView;

    try
    {
        const OUString sResourceURL (rxViewId->getResourceURL());

        if (sResourceURL == msCurrentSlidePreviewViewURL)
        {
            xView = CreateSlideShowView(rxViewId);
        }
        else if (sResourceURL == msNotesViewURL)
        {
            xView = CreateNotesView(rxViewId);
        }
        else if (sResourceURL == msNextSlidePreviewViewURL)
        {
            xView = CreateSlidePreviewView(rxViewId, rxAnchorPane);
        }
        else if (sResourceURL == msToolBarViewURL)
        {
            xView = CreateToolBarView(rxViewId);
        }
        else if (sResourceURL == msSlideSorterURL)
        {
            xView = CreateSlideSorterView(rxViewId);
        }
        else if (sResourceURL == msHelpViewURL)
        {
            xView = CreateHelpView(rxViewId);
        }

        // Activate it.
        CachablePresenterView* pView = dynamic_cast<CachablePresenterView*>(xView.get());
        if (pView != nullptr)
            pView->ActivatePresenterView();
    }
    catch (RuntimeException&)
    {
        xView = nullptr;
    }

    return xView;
}

Reference<XView> PresenterViewFactory::CreateSlideShowView(
    const Reference<XResourceId>& rxViewId) const
{
    if ( ! mxConfigurationController.is())
        return nullptr;
    if ( ! mxComponentContext.is())
        return nullptr;

    try
    {
        rtl::Reference<PresenterSlideShowView> xView;
        xView =
            new PresenterSlideShowView(
                mxComponentContext,
                rxViewId,
                mxControllerWeak.get(),
                mpPresenterController);
        xView->LateInit();
        return xView;
    }
    catch (RuntimeException&)
    {
        return nullptr;
    }
}

Reference<XView> PresenterViewFactory::CreateSlidePreviewView(
    const Reference<XResourceId>& rxViewId,
    const Reference<XPane>& rxAnchorPane) const
{
    Reference<XView> xView;

    if ( ! mxConfigurationController.is())
        return xView;
    if ( ! mxComponentContext.is())
        return xView;

    try
    {
        xView.set(
            static_cast<XWeak*>(new NextSlidePreview(
                mxComponentContext,
                rxViewId,
                rxAnchorPane,
                mpPresenterController)),
            UNO_QUERY_THROW);
    }
    catch (RuntimeException&)
    {
        xView = nullptr;
    }

    return xView;
}

Reference<XView> PresenterViewFactory::CreateToolBarView(
    const Reference<XResourceId>& rxViewId) const
{
    return new PresenterToolBarView(
        mxComponentContext,
        rxViewId,
        mxControllerWeak.get(),
        mpPresenterController);
}

Reference<XView> PresenterViewFactory::CreateNotesView(
    const Reference<XResourceId>& rxViewId) const
{
    Reference<XView> xView;

    if ( ! mxConfigurationController.is())
        return xView;
    if ( ! mxComponentContext.is())
        return xView;

    try
    {
        xView.set(static_cast<XWeak*>(
            new PresenterNotesView(
                mxComponentContext,
                rxViewId,
                mxControllerWeak.get(),
                mpPresenterController)),
            UNO_QUERY_THROW);
    }
    catch (RuntimeException&)
    {
        xView = nullptr;
    }

    return xView;
}

Reference<XView> PresenterViewFactory::CreateSlideSorterView(
    const Reference<XResourceId>& rxViewId) const
{
    Reference<XView> xView;

    if ( ! mxConfigurationController.is())
        return xView;
    if ( ! mxComponentContext.is())
        return xView;

    try
    {
        rtl::Reference<PresenterSlideSorter> pView (
            new PresenterSlideSorter(
                mxComponentContext,
                rxViewId,
                mxControllerWeak.get(),
                mpPresenterController));
        xView = pView.get();
    }
    catch (RuntimeException&)
    {
        xView = nullptr;
    }

    return xView;
}

Reference<XView> PresenterViewFactory::CreateHelpView(
    const Reference<XResourceId>& rxViewId) const
{
    return Reference<XView>(new PresenterHelpView(
        mxComponentContext,
        rxViewId,
        mxControllerWeak.get(),
        mpPresenterController));
}

//===== CachablePresenterView =================================================

CachablePresenterView::CachablePresenterView()
    : mbIsPresenterViewActive(true)
{
}

void CachablePresenterView::ActivatePresenterView()
{
    mbIsPresenterViewActive = true;
}

void CachablePresenterView::DeactivatePresenterView()
{
    mbIsPresenterViewActive = false;
}

void CachablePresenterView::ReleaseView()
{
}

}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

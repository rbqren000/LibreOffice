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

#ifndef INCLUDED_SDEXT_SOURCE_PRESENTER_PRESENTERVIEWFACTORY_HXX
#define INCLUDED_SDEXT_SOURCE_PRESENTER_PRESENTERVIEWFACTORY_HXX

#include "PresenterController.hxx"
#include <framework/ResourceFactory.hxx>
#include <com/sun/star/drawing/framework/XView.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <unotools/weakref.hxx>
#include <rtl/ref.hxx>
#include <memory>

namespace sd { class DrawController; }

namespace sdext::presenter {

/** Base class for presenter views that allows the view factory to store
    them in a cache and reuse deactivated views.
*/
class SAL_LOPLUGIN_ANNOTATE("crosscast") CachablePresenterView
{
public:
    virtual void ActivatePresenterView();

    /** Called when the view is put into a cache.  The view must not paint
        itself while being deactivated.
    */
    virtual void DeactivatePresenterView();

    /** Called before the view is disposed.  This gives the view the
        opportunity to trigger actions that may lead to (synchronous)
        callbacks that do not result in DisposedExceptions.
    */
    virtual void ReleaseView();

protected:
    bool mbIsPresenterViewActive;

    CachablePresenterView();

    ~CachablePresenterView() {}
};

/** Factory of the presenter screen specific views.  The supported set of
    views includes:
        a life view of the current slide,
        a static preview of the next slide,
        the notes of the current slide,
        a tool bar
*/
class PresenterViewFactory : public sd::framework::ResourceFactory
{
public:
    static constexpr OUString msCurrentSlidePreviewViewURL
        = u"private:resource/view/Presenter/CurrentSlidePreview"_ustr;
    static constexpr OUString msNextSlidePreviewViewURL
        = u"private:resource/view/Presenter/NextSlidePreview"_ustr;
    static constexpr OUString msNotesViewURL = u"private:resource/view/Presenter/Notes"_ustr;
    static constexpr OUString msToolBarViewURL = u"private:resource/view/Presenter/ToolBar"_ustr;
    static constexpr OUString msSlideSorterURL
        = u"private:resource/view/Presenter/SlideSorter"_ustr;
    static constexpr OUString msHelpViewURL = u"private:resource/view/Presenter/Help"_ustr;

    /** Create a new instance of this class and register it as resource
        factory in the drawing framework of the given controller.
        This registration keeps it alive.  When the drawing framework is
        shut down and releases its reference to the factory then the factory
        is destroyed.
    */
    static rtl::Reference<sd::framework::ResourceFactory> Create (
        const css::uno::Reference<css::uno::XComponentContext>& rxContext,
        const ::rtl::Reference<::sd::DrawController>& rxController,
        const ::rtl::Reference<PresenterController>& rpPresenterController);
    virtual ~PresenterViewFactory() override;

    virtual void disposing(std::unique_lock<std::mutex>&) override;

    // XResourceFactory

    virtual css::uno::Reference<css::drawing::framework::XResource>
        createResource (
            const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId) override;

    virtual void
        releaseResource (
            const css::uno::Reference<css::drawing::framework::XResource>& rxPane) override;

private:
    css::uno::Reference<css::uno::XComponentContext> mxComponentContext;
    rtl::Reference<sd::framework::ConfigurationController> mxConfigurationController;
    unotools::WeakReference<::sd::DrawController> mxControllerWeak;
    ::rtl::Reference<PresenterController> mpPresenterController;
    typedef ::std::pair<css::uno::Reference<css::drawing::framework::XView>,
        css::uno::Reference<css::drawing::framework::XPane> > ViewResourceDescriptor;
    typedef ::std::map<OUString, ViewResourceDescriptor> ResourceContainer;
    std::unique_ptr<ResourceContainer> mpResourceCache;

    PresenterViewFactory (
        const css::uno::Reference<css::uno::XComponentContext>& rxContext,
        const rtl::Reference<::sd::DrawController>& rxController,
        ::rtl::Reference<PresenterController> pPresenterController);

    void Register (const ::rtl::Reference<::sd::DrawController>& rxController);

    css::uno::Reference<css::drawing::framework::XView> CreateSlideShowView(
        const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId) const;

    css::uno::Reference<css::drawing::framework::XView> CreateSlidePreviewView(
        const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId,
        const css::uno::Reference<css::drawing::framework::XPane>& rxPane) const;

    css::uno::Reference<css::drawing::framework::XView> CreateToolBarView(
        const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId) const;

    css::uno::Reference<css::drawing::framework::XView> CreateNotesView(
        const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId) const;

    css::uno::Reference<css::drawing::framework::XView> CreateSlideSorterView(
        const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId) const;

    css::uno::Reference<css::drawing::framework::XView> CreateHelpView(
        const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId) const;

    css::uno::Reference<css::drawing::framework::XResource> GetViewFromCache (
        const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId,
        const css::uno::Reference<css::drawing::framework::XPane>& rxAnchorPane) const;
    css::uno::Reference<css::drawing::framework::XResource> CreateView(
        const css::uno::Reference<css::drawing::framework::XResourceId>& rxViewId,
        const css::uno::Reference<css::drawing::framework::XPane>& rxAnchorPane);
};

}

#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

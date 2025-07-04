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

#ifndef INCLUDED_SW_SOURCE_CORE_ACCESS_ACCHEADERFOOTER_HXX
#define INCLUDED_SW_SOURCE_CORE_ACCESS_ACCHEADERFOOTER_HXX

#include "acccontext.hxx"

class SwHeaderFrame;
class SwFooterFrame;

class SwAccessibleHeaderFooter : public SwAccessibleContext
{
protected:
    virtual ~SwAccessibleHeaderFooter() override;

public:
    SwAccessibleHeaderFooter(std::shared_ptr<SwAccessibleMap> const& pInitMap,
                             const SwHeaderFrame* pHdFrame);
    SwAccessibleHeaderFooter(std::shared_ptr<SwAccessibleMap> const& pInitMap,
                             const SwFooterFrame* pFtFrame);

    // XAccessibleContext

    /// Return this object's description.
    virtual OUString SAL_CALL getAccessibleDescription() override;

    // XServiceInfo

    /** Returns an identifier for the implementation of this object. */
    virtual OUString SAL_CALL getImplementationName() override;

    virtual css::uno::Sequence<OUString> SAL_CALL getSupportedServiceNames() override;

    // XAccessibleComponent
    sal_Int32 SAL_CALL getBackground() override;
};

#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <test/unoapi_test.hxx>

#include <boost/property_tree/json_parser.hpp>

#include <com/sun/star/drawing/XDrawView.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>

#include <sfx2/app.hxx>
#include <sfx2/sfxsids.hrc>
#include <sfx2/viewfrm.hxx>
#include <svl/itemset.hxx>
#include <svl/intitem.hxx>
#include <sfx2/request.hxx>
#include <sfx2/bindings.hxx>
#include <sfx2/lokhelper.hxx>
#include <sfx2/sfxbasemodel.hxx>
#include <tools/json_writer.hxx>
#include <rtl/ustrbuf.hxx>
#include <comphelper/base64.hxx>

using namespace com::sun::star;

/// Covers sfx2/source/view/ fixes.
class Sfx2ViewTest : public UnoApiTest
{
public:
    Sfx2ViewTest()
        : UnoApiTest(u"/sfx2/qa/cppunit/data/"_ustr)
    {
    }

    void setUp() override
    {
        UnoApiTest::setUp();
        MacrosTest::setUpX509(m_directories, "sfx2_view");
    }
};

CPPUNIT_TEST_FIXTURE(Sfx2ViewTest, testReloadPage)
{
    // Load a document, which has 2 pages.
    loadFromFile(u"reload-page.odg");

    // Reload, and request to start on page 2.
    SfxViewFrame* pFrame = SfxViewFrame::Current();
    SfxAllItemSet aSet(SfxGetpApp()->GetPool());
    aSet.Put(SfxInt32Item(SID_PAGE_NUMBER, 1));
    SfxRequest aReq(SID_RELOAD, SfxCallMode::SLOT, aSet);
    pFrame->ExecReload_Impl(aReq);
    SfxObjectShell* pDocSh = SfxObjectShell::Current();
    CPPUNIT_ASSERT(pDocSh);
    uno::Reference<frame::XModel> xModel = pDocSh->GetBaseModel();
    mxComponent = xModel;

    // Check the current page after reload.
    uno::Reference<drawing::XDrawView> xController(xModel->getCurrentController(), uno::UNO_QUERY);
    uno::Reference<beans::XPropertySet> xPage(xController->getCurrentPage(), uno::UNO_QUERY);
    sal_Int32 nPage{};
    xPage->getPropertyValue(u"Number"_ustr) >>= nPage;

    // Without the accompanying fix in place, this test would have failed with:
    // - Expected: 2
    // - Actual  : 1
    // i.e. the document was opened on page 1, not page 2, SID_PAGE_NUMBER was ignored.
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(2), nPage);
}

CPPUNIT_TEST_FIXTURE(Sfx2ViewTest, testLokHelperExtractCertificates)
{
    std::string signatureCa = R"(-----BEGIN CERTIFICATE-----
foo
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
bar
-----END CERTIFICATE-----)";

    std::vector<std::string> aRet = SfxLokHelper::extractCertificates(signatureCa);

    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(2), aRet.size());
    CPPUNIT_ASSERT_EQUAL(std::string("\nfoo\n"), aRet[0]);
    CPPUNIT_ASSERT_EQUAL(std::string("\nbar\n"), aRet[1]);
}

#ifdef UNX
CPPUNIT_TEST_FIXTURE(Sfx2ViewTest, testLokHelperAddCertifices)
{
    // Given a loaded and signed document, CA is not trusted by default:
    loadFromFile(u"signed.odt");
    auto pBaseModel = dynamic_cast<SfxBaseModel*>(mxComponent.get());
    SfxObjectShell* pObjectShell = pBaseModel->GetObjectShell();
    CPPUNIT_ASSERT_EQUAL(SignatureState::NOTVALIDATED, pObjectShell->GetDocumentSignatureState());

    // When trusting the CA:
    OUString aCaUrl = createFileURL(u"ca.pem");
    SvFileStream aCaStream(aCaUrl, StreamMode::READ);
    std::string aCa;
    aCa = read_uInt8s_ToOString(aCaStream, aCaStream.remainingSize());
    std::vector<std::string> aCerts = SfxLokHelper::extractCertificates(aCa);
    SfxLokHelper::addCertificates(aCerts);

    // Then make sure the signature state is updated:
    // Without the accompanying fix in place, this test would have failed with:
    // - Expected: 1 (OK)
    // - Actual  : 4 (SignatureState::NOTVALIDATED)
    // i.e. the signature status for an opened document was not updated when trusting a CA.
    CPPUNIT_ASSERT_EQUAL(SignatureState::OK, pObjectShell->GetDocumentSignatureState());
}

CPPUNIT_TEST_FIXTURE(Sfx2ViewTest, testLokHelperCommandValuesSignature)
{
    // Given an unsigned PDF file:
    loadFromFile(u"unsigned.pdf");

    // When extracting hashes:
    tools::JsonWriter aWriter;
    SfxLokHelper::getCommandValues(aWriter, ".uno:Signature");
    OString aJson = aWriter.finishAndGetAsOString();

    // Then make sure that we get a signature time and a hash:
    CPPUNIT_ASSERT(SfxLokHelper::supportsCommand(u"Signature"));
    std::stringstream aStream{ std::string(aJson) };
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    // Non-zero timestamp:
    auto it = aTree.find("signatureTime");
    CPPUNIT_ASSERT(it != aTree.not_found());
    auto nSignatureTime = it->second.get_value<sal_Int64>();
    CPPUNIT_ASSERT(nSignatureTime != 0);
    // Base64 encoded hash, that has the SHA-256 length:
    it = aTree.find("digest");
    CPPUNIT_ASSERT(it != aTree.not_found());
    auto aDigest = OUString::fromUtf8(it->second.get_value<std::string>());
    uno::Sequence<sal_Int8> aBytes;
    comphelper::Base64::decode(aBytes, aDigest);
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(32), aBytes.getLength());
}
#endif

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

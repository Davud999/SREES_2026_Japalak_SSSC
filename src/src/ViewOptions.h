#pragma once
#include <gui/View.h>
#include <gui/Label.h>
#include <gui/Button.h>
#include <gui/LineEdit.h>
#include <gui/NumericEdit.h>
#include <gui/GridLayout.h>
#include <gui/GridComposer.h>
#include <gui/HorizontalLayout.h>
#include <gui/FileDialog.h>
#include <xml/Writer.h>
#include <xml/DOMParser.h>
#include "EQPlugin.h"

class ViewOptions : public gui::View
{
private:
protected:
    gui::Label _lblName;
    gui::LineEdit _editName;
    gui::Label _lblMaxIter;
    gui::Label _lbldT;
    gui::Label _llblEndT;
    gui::NumericEdit _neMaxIter;
    gui::NumericEdit _neDeltaTime;
    gui::NumericEdit _neEndTime;
    gui::Label _lblSsscFrom;
    gui::Label _lblSsscTo;
    gui::Label _lblSsscX;
    gui::Label _lblSsscV;
    gui::Label _lblSsscAngle;
    gui::NumericEdit _neSsscFrom;
    gui::NumericEdit _neSsscTo;
    gui::NumericEdit _neSsscX;
    gui::NumericEdit _neSsscV;
    gui::NumericEdit _neSsscAngle;
    gui::Label _lblXmlStatus;
    gui::LineEdit _editXmlStatus;
    gui::Button _btnLoadXml;
    gui::Button _btnSaveXml;
    gui::HorizontalLayout _hlXmlButtons;
    gui::GridLayout _gl;
    Options _options;

    td::UINT4 _wndID;
protected:
    // ---- konfiguracijski XML fajl (obavezan zahtjev iz uputstva) ----
    // Format:
    // <SSSCConfig maxIter="50" ssscFromBus="5" ssscToBus="6"
    //             ssscReactance="0.1" ssscVoltage="0.05" ssscAngle="0.1"/>
    void saveToXml(const td::String& fileName)
    {
        int maxIterVal = int(_neMaxIter.getValue().i4Val());
        int fromBusVal = int(_neSsscFrom.getValue().i4Val());
        int toBusVal = int(_neSsscTo.getValue().i4Val());
        float xVal = float(_neSsscX.getValue().r4Val());
        float vVal = float(_neSsscV.getValue().r4Val());
        float aVal = float(_neSsscAngle.getValue().r4Val());

        xml::Writer w(fileName);
        w.startDocument();
        w.startNode("SSSCConfig");
        w.attribute("maxIter", maxIterVal);
        w.attribute("ssscFromBus", fromBusVal);
        w.attribute("ssscToBus", toBusVal);
        w.attribute("ssscReactance", xVal);
        w.attribute("ssscVoltage", vVal);
        w.attribute("ssscAngle", aVal);
        w.comment("Konfiguracija SSSC konvertora - koji cvor (bus) je From/To, "
                  "i parametri uredjaja. Generisano iz Options taba.");
        w.endNode();
        w.endDocument();
        _editXmlStatus = "INFO! XML config saved.";
    }

    void loadFromXml(const td::String& fileName)
    {
        xml::FileParser parser;
        if (!parser.parseFile(fileName))
        {
            _editXmlStatus = "ERROR! Cannot parse XML file!";
            return;
        }
        auto root = parser.getRootNode();
        if (root->getName().cCompare("SSSCConfig") != 0)
        {
            _editXmlStatus = "ERROR! Not a valid SSSCConfig XML file!";
            return;
        }
        td::INT4 maxIter = 50;
        td::INT4 fromBus = 5, toBus = 6;
        float xVal = 0.1f, vVal = 0.05f, aVal = 0.1f;
        root.getAttribValue("maxIter", maxIter);
        root.getAttribValue("ssscFromBus", fromBus);
        root.getAttribValue("ssscToBus", toBus);
        root.getAttribValue("ssscReactance", xVal);
        root.getAttribValue("ssscVoltage", vVal);
        root.getAttribValue("ssscAngle", aVal);

        _neMaxIter.setValue(maxIter);
        _neSsscFrom.setValue(fromBus);
        _neSsscTo.setValue(toBus);
        _neSsscX.setValue(xVal);
        _neSsscV.setValue(vVal);
        _neSsscAngle.setValue(aVal);
        _editXmlStatus = "INFO! XML config loaded.";
    }

    void handleXmlActions()
    {
        _btnLoadXml.onClick([this]{
            gui::OpenFileDialog::show(this, tr("openXmlConfig"), "*.xml", _wndID + 3000, [this](gui::FileDialog* pDlg)
            {
                auto status = pDlg->getStatus();
                if (status == gui::FileDialog::Status::OK)
                {
                    td::String fileName = pDlg->getFileName();
                    if (fileName.isEmpty()) return;
                    loadFromXml(fileName);
                }
            });
        });
        _btnSaveXml.onClick([this]{
            gui::SaveFileDialog::show(this, tr("saveXmlConfig"), "*.xml", _wndID + 4000, [this](gui::FileDialog* pDlg)
            {
                auto status = pDlg->getStatus();
                if (status == gui::FileDialog::Status::OK)
                {
                    td::String fileName = pDlg->getFileName();
                    if (fileName.isEmpty()) return;
                    saveToXml(fileName);
                }
            });
        });
    }

public:
    ViewOptions()
    : _lblName(tr("Model name:"))
    , _lblMaxIter(tr("Max iters:"))
    , _lbldT(tr("dT:"))
    , _llblEndT(tr("End time:"))
    , _neMaxIter(td::int4)
    , _neDeltaTime(td::real4, gui::LineEdit::Messages::DoNotSend, false, tr("Integration time"), 3)
    , _neEndTime(td::real4, gui::LineEdit::Messages::DoNotSend, true, tr("Integration time"), 3)
    , _lblSsscFrom(tr("SSSC From bus:"))
    , _lblSsscTo(tr("SSSC To bus:"))
    , _lblSsscX(tr("SSSC X [p.u.]:"))
    , _lblSsscV(tr("SSSC V [p.u.]:"))
    , _lblSsscAngle(tr("SSSC angle [rad]:"))
    , _neSsscFrom(td::int4)
    , _neSsscTo(td::int4)
    , _neSsscX(td::real4, gui::LineEdit::Messages::DoNotSend, false, tr("SSSC reactance"), 4)
    , _neSsscV(td::real4, gui::LineEdit::Messages::DoNotSend, false, tr("SSSC voltage"), 4)
    , _neSsscAngle(td::real4, gui::LineEdit::Messages::DoNotSend, false, tr("SSSC angle"), 4)
    , _lblXmlStatus(tr("XML config:"))
    , _btnLoadXml(tr("Load XML..."))
    , _btnSaveXml(tr("Save XML..."))
    , _hlXmlButtons(2)
    , _gl(8,4)
    {
        _editName = "SSSC power flow model (case9/case30/case118/case300)";
        _neMaxIter.setValue(td::INT4(50));
        _neDeltaTime.setValue(0.001f);
        _neEndTime.setValue(60.f);
        _neSsscFrom.setValue(td::INT4(5));
        _neSsscTo.setValue(td::INT4(6)); // mora biti postojeca grana u mrezi
        _neSsscX.setValue(0.1f);
        _neSsscV.setValue(0.05f);
        _neSsscAngle.setValue(0.1f);
        _editXmlStatus.setAsReadOnly();

        gui::GridComposer gc(_gl);
        gc.appendRow(_lblName); gc.appendCol(_editName, 0);
        gc.appendRow(_lblMaxIter) << _neMaxIter;
        gc.appendRow(_lbldT) << _neDeltaTime << _llblEndT << _neEndTime;
        gc.appendRow(_lblSsscFrom) << _neSsscFrom << _lblSsscTo << _neSsscTo;
        gc.appendRow(_lblSsscX) << _neSsscX << _lblSsscV << _neSsscV;
        gc.appendRow(_lblSsscAngle) << _neSsscAngle;
        _hlXmlButtons << _btnLoadXml << _btnSaveXml;
        gc.appendRow(_lblXmlStatus) << _editXmlStatus;
        gc.appendRow(_hlXmlButtons, 0); //0:span to end

        setLayout(&_gl);
        handleXmlActions();
    }

    const Options& getOptions()
    {
        _options._modelName = _editName.getText();
        _options._maxIter = _neMaxIter.getValue().i4Val();
        _options._dTime = _neDeltaTime.getValue().r4Val();
        _options._endTime = _neEndTime.getValue().r4Val();
        _options._ssscFromBus = _neSsscFrom.getValue().i4Val();
        _options._ssscToBus = _neSsscTo.getValue().i4Val();
        _options._ssscReactance = _neSsscX.getValue().r4Val();
        _options._ssscVoltage = _neSsscV.getValue().r4Val();
        _options._ssscAngle = _neSsscAngle.getValue().r4Val();
        return _options;
    }

};


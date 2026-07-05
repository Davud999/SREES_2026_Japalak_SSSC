#pragma once
#include <compiler/Definitions.h>
#include <sc/IPlugin.h>
#include <gui/LineEdit.h>
#include <functional>

#ifdef MU_WINDOWS
	#ifdef PLUGIN_EXPORTS
	#define PLUGIN_API __declspec(dllexport)
	#else
	#define PLUGIN_API __declspec(dllimport)
	#endif
#else
	#ifdef PLUGIN_EXPORTS
    #define PLUGIN_API __attribute__((visibility("default")))
	#else
    #define PLUGIN_API
	#endif
#endif


using Options = struct _Options
{
    td::String _modelName;
    td::INT4 _maxIter;
    float _dTime;
    float _endTime;
    td::INT4 _ssscFromBus;   // ID bus-a (from) na kojem sjedi SSSC - podesivo, ne hardkodirano
    td::INT4 _ssscToBus;     // ID bus-a (to) na kojem sjedi SSSC
    float _ssscReactance;    // X_sssc [p.u.]
    float _ssscVoltage;      // V_sssc - injektovani napon SSSC-a [p.u.] (fiksni setpoint)
    float _ssscAngle;        // δ_sssc - injektovani ugao SSSC-a [rad] (fiksni setpoint)
};

void onClosedPluginWindow();

// status je td::String (ne gui::LineEdit&) da bi funkcija bila bezbjedna za
// pozivanje iz pozadinskog (worker) thread-a - GUI kontrole se smiju mijenjati
// samo iz glavnog thread-a. onProgress se poziva periodicno (0.0-1.0) da bi
// GUI mogao prikazati progres konverzije u realnom vremenu dok radi drugi thread.
bool createModel(const td::String& inputFileName, const td::String& outFileName,
                  sc::IPlugin* pIPlugin, const Options& options, td::String& status,
                  std::function<void(double)> onProgress = nullptr); //in EQPlugin.cpp


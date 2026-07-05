#include "EQPlugin.h"
#include <sc/IPlugin.h>
#include "WindowPlugin.h"
#include <td/StringUtils.h>
#include <dense/Matrix.h>
#include <mu/ScopedCLocale.h>
#include <complex>
#include <map>

// ---- NOVO: strukture i parser ----
struct Bus {
    int _id;
    int _type;    // 1=PQ, 2=PV, 3=slack
    double _pd, _qd;
    double _vm, _va;
    double _vmax, _vmin;
};

struct Branch {
    int _from, _to;
    double _r, _x, _b;
};

struct Gen {
    int _bus;
    double _pg, _qg;
    double _vg;
};

static bool parseMATPOWER(const std::string& fileName,
                           std::vector<Bus>& buses,
                           std::vector<Branch>& branches,
                           std::vector<Gen>& gens,
                           double& baseMVA)
{
    std::ifstream f(fileName);
    if (!f.is_open()) return false;

    std::string line;
    int section = 0; // 0=none, 1=bus, 2=gen, 3=branch

    while (std::getline(f, line)) {
        // ukloni whitespace
        if (line.empty() || line[0] == '%') continue;

        if (line.find("mpc.baseMVA") != std::string::npos) {
            sscanf(line.c_str(), "mpc.baseMVA = %lf", &baseMVA);
            continue;
        }
        if (line.find("mpc.bus = [") != std::string::npos) { section = 1; continue; }
        if (line.find("mpc.gen = [") != std::string::npos) { section = 2; continue; }
        if (line.find("mpc.branch = [") != std::string::npos) { section = 3; continue; }
        if (line.find("];") != std::string::npos) { section = 0; continue; }

        if (section == 1) {
            Bus b;
            sscanf(line.c_str(), "%d %d %lf %lf %*f %*f %*f %lf %lf %*f %*f %lf %lf",
                   &b._id, &b._type, &b._pd, &b._qd, &b._vm, &b._va, &b._vmax, &b._vmin);
            buses.push_back(b);
        }
        else if (section == 2) {
            Gen g;
            sscanf(line.c_str(), "%d %lf %lf %*f %*f %lf",
                   &g._bus, &g._pg, &g._qg, &g._vg);
            gens.push_back(g);
        }
        else if (section == 3) {
            Branch br;
            sscanf(line.c_str(), "%d %d %lf %lf %lf",
                   &br._from, &br._to, &br._r, &br._x, &br._b);
            branches.push_back(br);
        }
    }
    return !buses.empty();
}

// Y matrica se OBAVEZNO gradi kao natID dense::CmplxMatrix
// (uputstvo: sve matrice moraju biti natID dense ili sparse, nikad std:: kontejneri)
static void buildYMatrix(const std::vector<Bus>& buses,
                          const std::vector<Branch>& branches,
                          int nBus,
                          dense::CmplxMatrix& Y)
{
    Y.setDimension(nBus, nBus);
    auto y = Y.getManipulator();

    // inicijalizacija na nulu
    for (int i = 0; i < nBus; ++i)
        for (int j = 0; j < nBus; ++j)
            y(i, j) = td::cmplx(0.0, 0.0);

    // ID bus-a NIJE nuzno == pozicija u nizu + 1 (case300 ima "rupe" u
    // numeraciji busova!). Zato gradimo ID -> indeks mapu i koristimo je,
    // umjesto pretpostavke "br._from - 1" koja je pucala (out-of-bounds
    // pristup matrici -> korupcija memorije -> crash cijele aplikacije).
    std::map<int,int> idToIdx;
    for (int k = 0; k < nBus; ++k)
        idToIdx[buses[k]._id] = k;

    for (const auto& br : branches) {
        auto itFrom = idToIdx.find(br._from);
        auto itTo   = idToIdx.find(br._to);
        if (itFrom == idToIdx.end() || itTo == idToIdx.end())
            continue; // grana referencira nepostojeci bus - preskoci sigurno
        int i = itFrom->second;
        int j = itTo->second;

        // serijska admitansa grane
        double denom = br._r*br._r + br._x*br._x;
        td::cmplx ySeries(br._r/denom, -br._x/denom);

        // šant admitansa (charging)
        td::cmplx yShunt(0.0, br._b/2.0);

        // dodaj u Y matricu
        y(i, i) = y(i, i) + ySeries + yShunt;
        y(j, j) = y(j, j) + ySeries + yShunt;
        y(i, j) = y(i, j) - ySeries;
        y(j, i) = y(j, i) - ySeries;
    }
}

static void generateDmodl(const std::vector<Bus>& buses,
                           const std::vector<Branch>& branches,
                           const std::vector<Gen>& gens,
                           dense::CmplxMatrix& Y,
                           double baseMVA,
                           int ssscFrom, int ssscTo,
                           double X_sssc,
                           double vSsscSet, double deltaSsscSet,
                           arch::MemoryOut& memOut,
                           const std::function<void(double)>& onProgress)
{
    td::MutableString s;
    s.reserve(8192);
    int nBus = (int)buses.size();
    auto y = Y.getManipulator(); // pristup elementima natID CmplxMatrix

    // pronadi slack bus index
    int slackIdx = 0;
    for (int i = 0; i < nBus; i++)
        if (buses[i]._type == 3) { slackIdx = i; break; }
    int slackId = buses[slackIdx]._id;

    // --- Header ---
    memOut.put("Header:\n");
    memOut.put("\tmaxIter = 50\n");
    memOut.put("\treport = All\n");
    memOut.put("end\n\n");

    // --- Model ---
    s.appendFormat("Model [type=NL domain=real eps=1e-6 name=\"PF_SSSC_%dbus\"]:\n", nBus);
    memOut.put(s.c_str(), s.length()); s.reset();

    // --- Vars ---
    memOut.put("Vars [out=true]:\n\t");
    bool first = true;
    // kutovi za sve osim slack-a
    for (int i = 0; i < nBus; i++) {
        if (buses[i]._type == 3) continue;
        if (!first) s.append("; ");
        first = false;
        s.appendFormat("δ_%d=δ_%d", buses[i]._id, slackId);
    }
    s.append("\n\t");
    // naponi samo za PQ čvorove
    first = true;
    for (int i = 0; i < nBus; i++) {
        if (buses[i]._type != 1) continue; // samo PQ
        if (!first) s.append("; ");
        first = false;
        s.appendFormat("v_%d=v_%d", buses[i]._id, slackId);
    }
    s.append("\n");
    memOut.put(s.c_str(), s.length()); s.reset();

    // --- Params ---
    memOut.put("Params:\n\t");
    // SSSC kontrolni parametri (injektovani napon/ugao) - FIKSNI, ne nepoznate.
    // Razlog: ako bi V_sssc/δ_sssc bili u Vars (nepoznate), sistem bi imao
    // 2 nepoznate vise nego jednacina (SSSC clan ne dodaje nove jednacine,
    // samo modifikuje postojece P/Q jednacine na from/to busu) -> singularna
    // ili neodredjena matrica, za SVAKU velicinu mreze (case9/30/118/300).
    s.appendFormat("V_sssc=%.4f; δ_sssc=%.4f //SSSC injected voltage and angle (fixed setpoint)\n\t",
                   vSsscSet, deltaSsscSet);
    // slack parametri
    s.appendFormat("δ_%d=0 [out=true] //slack angle\n\t", slackId);
    s.appendFormat("v_%d=%.4f [out=true] //slack voltage\n\t", slackId, buses[slackIdx]._vm);
    // PV čvorovi - v je parametar
    for (int i = 0; i < nBus; i++) {
        if (buses[i]._type == 2) {
            s.appendFormat("v_%d=%.4f; ", buses[i]._id, buses[i]._vm);
        }
    }
    s.append("\n\t");
    // Y matrica elementi - samo nenulti
    for (int i = 0; i < nBus; i++) {
        int bi = buses[i]._id;
        // dijagonalni element
        double aYii = std::abs(y(i, i));
        double thii = std::arg(y(i, i));
        s.appendFormat("aY%d%d=%.8f; θ_%d%d=%.8f\n\t", bi, bi, aYii, bi, bi, thii);
        // van-dijagonalni elementi
        for (int j = 0; j < nBus; j++) {
            if (i == j) continue;
            if (std::abs(y(i, j)) < 1e-10) continue;
            int bj = buses[j]._id;
            double aYij = std::abs(y(i, j));
            double thij = std::arg(y(i, j));
            s.appendFormat("aY%d%d=%.8f; θ_%d%d=%.8f\n\t", bi, bj, aYij, bi, bj, thij);
        }
        memOut.put(s.c_str(), s.length()); s.reset();
        if (onProgress) onProgress(0.30 + 0.30 * double(i + 1) / double(nBus));
    }
    // SSSC parametri
    s.appendFormat("X_sssc=%.4f //SSSC reactance\n", X_sssc);
    memOut.put(s.c_str(), s.length()); s.reset();

    // --- Injektirane snage (P i Q) ---
    memOut.put("\t// Injected powers (generation - load) in p.u.\n\t");
    for (int i = 0; i < nBus; i++) {
        if (buses[i]._type == 3) continue;
        int bi = buses[i]._id;
        double Pinj = -buses[i]._pd / baseMVA;
        double Qinj = -buses[i]._qd / baseMVA;
        // dodaj generaciju
        for (const auto& g : gens) {
            if (g._bus == bi) {
                Pinj += g._pg / baseMVA;
                Qinj += g._qg / baseMVA;
            }
        }
        s.appendFormat("P%d_inj=%.6f; Q%d_inj=%.6f\n\t", bi, Pinj, bi, Qinj);
        memOut.put(s.c_str(), s.length()); s.reset();
    }

    // --- NLEs ---
    memOut.put("NLEs:\n");
    for (int i = 0; i < nBus; i++) {
        if (buses[i]._type == 3) continue; // slack nema jednacine
        int bi = buses[i]._id;

        // P jednacina: v_i^2*aYii*cos(θii) + v_i*(sum_j≠i aYij*v_j*cos(δi-θij-δj)) = Pinj
        s.appendFormat("\t// Bus %d (%s) - P equation\n\t",
                       bi, buses[i]._type==2 ? "PV" : "PQ");
        s.appendFormat("v_%d^2*aY%d%d*cos(θ_%d%d)", bi, bi, bi, bi, bi);
        for (int j = 0; j < nBus; j++) {
            if (i == j) continue;
            if (std::abs(y(i, j)) < 1e-10) continue;
            int bj = buses[j]._id;
            s.appendFormat(" + v_%d*aY%d%d*v_%d*cos(δ_%d-θ_%d%d-δ_%d)",
                           bi, bi, bj, bj, bi, bi, bj, bj);
        }
        // SSSC doprinos na P (power injection model - zavisi od v_bi, δ_bi,
        // suprotan predznak na from/to strani zbog ocuvanja snage kroz uredjaj)
        if (bi == ssscFrom) {
            s.appendFormat(" + v_%d*V_sssc/X_sssc*sin(δ_%d-δ_sssc)", bi, bi);
        } else if (bi == ssscTo) {
            s.appendFormat(" - v_%d*V_sssc/X_sssc*sin(δ_%d-δ_sssc)", bi, bi);
        }
        s.appendFormat(" = P%d_inj\n", bi);
        memOut.put(s.c_str(), s.length()); s.reset();

        // Q jednacina samo za PQ čvorove
        if (buses[i]._type == 1) {
            s.appendFormat("\t// Bus %d (PQ) - Q equation\n\t", bi);
            s.appendFormat("-v_%d^2*aY%d%d*sin(θ_%d%d)", bi, bi, bi, bi, bi);
            for (int j = 0; j < nBus; j++) {
                if (i == j) continue;
                if (std::abs(y(i, j)) < 1e-10) continue;
                int bj = buses[j]._id;
                s.appendFormat(" + v_%d*aY%d%d*v_%d*sin(δ_%d-θ_%d%d-δ_%d)",
                               bi, bi, bj, bj, bi, bi, bj, bj);
            }
            // SSSC doprinos na Q (isti princip kao za P)
            if (bi == ssscFrom) {
                s.appendFormat(" - v_%d*V_sssc/X_sssc*cos(δ_%d-δ_sssc)", bi, bi);
            } else if (bi == ssscTo) {
                s.appendFormat(" + v_%d*V_sssc/X_sssc*cos(δ_%d-δ_sssc)", bi, bi);
            }
            s.appendFormat(" = Q%d_inj\n", bi);
            memOut.put(s.c_str(), s.length()); s.reset();
        }
        // NAPOMENA: PV cvor NE dobija dodatnu "V equation" - v_i je vec
        // fiksiran kao PARAMETAR u Params sekciji (nije nepoznata), pa bi
        // jednacina "v_i = const" bila red pun nula u Jacobianu -> singularna
        // matrica, garantovano, za SVAKI case fajl sa PV cvorovima (case9,
        // case30, case118, case300 podjednako). Zato je ovdje NEMA.
        if (onProgress) onProgress(0.60 + 0.35 * double(i + 1) / double(nBus));
    }
    memOut.put("end\n");
}
//---- DOVDE je NOVO -----

class Plugin : public sc::IPlugin
{
    MemoryArchiveContainer _outArchives;
    WindowPlugin* _pWnd = nullptr;
public:
    Plugin()
    {
        //dont change this
        for (size_t i=0; i< size_t(ArchType::NA); ++i)
            _outArchives[i] = nullptr;
    }
    
    void show(gui::Window* parentWnd, MemoryArchiveContainer& archives, td::UINT4 wndID, const sc::IPlugin::Cleaner& cleaner, const sc::IPlugin::CallBack& onComplete) override final
    {
        //dont change this
        for (size_t i=0; i< size_t(ArchType::NA); ++i)
            _outArchives[i] = archives[i];
        
        if (_pWnd)
            _pWnd->setFocus();
        else
        {
            _pWnd = new WindowPlugin(parentWnd, this, onComplete, cleaner, wndID);
            _pWnd->open();
        }
    }
    
    td::String getMenuName() const override final
    {
        return "SSSC Converter";
    }
    
    arch::MemoryOut* getArchive(sc::IPlugin::ArchType type) override final
    {
        //dont change this
        auto iType = size_t(type);
        if (iType >= getMaxSupportedArchiveParts())
            return nullptr;
        
        return _outArchives[size_t(type)];
    }
    
    MemoryArchiveContainer& getArchives() override final
    {
        //dont change this
        return _outArchives;
    }
    
    td::String getOutFileName() const override final
    {
        //dont change this
        assert(_pWnd);
        return _pWnd->getOutFileName();
    }
    
    size_t getMaxSupportedArchiveParts() const override final
    {
        return size_t(ArchType::NA); //don't change this
    }
    
    ModelType getModelType() const override final
    {
        //NOTE: adjust this to match your converter type
        return ModelType::DAE;
    }
    
    void onClosedPluginWindow()
    {
        //dont change this
        _pWnd = nullptr;
    }
    
};

static Plugin s_plugin;

void onClosedPluginWindow()
{
    s_plugin.onClosedPluginWindow();
}

//Plugin requires extern C
extern "C"
{

PLUGIN_API sc::IPlugin* getPluginInterface()
{
    return &s_plugin;
}
}

enum class Format {Unknown=0, Plain, Matlab};

//Converter implemenation
static bool loadMatrices(const td::String& fileName, dense::DblMatrix matrices[3], gui::LineEdit& status)
{
    fo::InFile inFile;
    if (!fo::openExistingBinaryFile(inFile, fileName))
        return false;
    
    cnt::PushBackVector<td::String> cntTokens;
    cntTokens.reserve(16);
    cnt::PushBackVector<td::String> cntRowTokens;
    cntRowTokens.reserve(16);
    cnt::PushBackVector<td::String> cntColTokens;
    cntColTokens.reserve(16);
    
    Format format = Format::Unknown;
    
    fo::LineNormal buffer;
    int line = 0;
    td::UINT4 nRows = 0;
    int iCurrRow = -1;
    int iMatrix = 0;
    
    
    while (fo::getLine(inFile, buffer))
    {
        ++line;
        const char* pBuff = buffer.c_str();
        auto buffLen = buffer.length();
        
        pBuff = td::findFirstNonWhiteSpace(pBuff);
        
        if (!pBuff)
            continue;
        //check first nonwhite space character
        char ch = *pBuff;
        if (ch == 0)
            continue;
        if (ch == '#')  //comment (MATLAB style)
            continue;
        if (ch == '/')  //comment c++ style
            continue;
        
        td::String str(pBuff);
        
        if (format == Format::Unknown)
        {
            int nEq = str.countAppearance('=');
            if (nEq == 0)
            {
                status = "ERROR! Missing '=' sign in format definition!";
                return false;
            }
                
            str.split('=', cntTokens);
            auto nToks = cntTokens.size();
            if (nToks != 2)
            {
                status = "ERROR! Incomplete format declaration in input";
                return false;
            }
            const auto& fmt = cntTokens[0];
            if (!fmt.compareConstStr("format"))
                if (nToks != 2)
                {
                    status = "ERROR! Format declaration must start with 'format ='";
                    return false;
                }
            const auto& fmtType = cntTokens[1];
            if (fmtType.compareCI("MATLAB"))
                format = Format::Matlab;
            else if (fmtType.compareCI("Plain"))
                format = Format::Plain;
            else
            {
                status = "ERROR! Unknown format type. Supported: MATLAB and Plain'";
                return false;
            }
        }
        else
        {
            if (iCurrRow >= int(nRows))
            {
                nRows = 0;
                iCurrRow = -1;
            }
            
            if (iCurrRow < 0)
            {
                int nEq = str.countAppearance('=');
                if (nEq == 0)
                {
                    status = "ERROR! Missing '=' sign in matrix definition!";
                    return false;
                }
                str.split('=', cntTokens);
                auto nToks = cntTokens.size();
                if (nToks != 2)
                {
                    status = "ERROR! Incomplete matrix declaration!";
                    return false;
                }
                const auto& fmt = cntTokens[0];
                if (fmt.compareCI("M"))
                {
                    iMatrix = 0;
                }
                else if (fmt.compareCI("K"))
                {
                    iMatrix = 1;
                }
                else if (fmt.compareCI("C"))
                {
                    iMatrix = 2;
                }
                else
                {
                    status = "ERROR! Unknown matrix name! Supported names: M,K,C";
                    return false;
                }
                if (matrices[iMatrix].getNoOfRows() > 0)
                {
                    status = "ERROR! Matrix entered twice";
                    return false;
                }
                //check second part
                //extract rows
                const auto& secondPart = cntTokens[1];
                auto sec2 = secondPart.replace("[", " ");
                auto sec3 = sec2.replace("]", " ");
                sec3.split(";", cntRowTokens);
                auto nRowsToProcess = cntRowTokens.size();
                if (iCurrRow < 0)
                    iCurrRow = 0;
                for (td::UINT4 iRow = 0; iRow < nRowsToProcess; ++iRow)
                {
                    if (iCurrRow > nRows)
                    {
                        status = "ERROR! Matrix size mismatch";
                        return false;
                    }
                    const auto& row = cntRowTokens[iRow];
                    row.split(" ,", cntColTokens);
                    td::UINT4 nCols = td::UINT4(cntColTokens.size());
                    if (nRows == 0)
                    {
                        nRows = nCols;
                        matrices[iMatrix].reserve(nRows, nRows);
                    }
                    else if (nRows != nCols)
                    {
                        status = "ERROR! Matrix has to be quadratic";
                        return false;
                    }
                    
                    auto mat = matrices[iMatrix].getManipulator();
                    for (td::UINT4 iCol=0; iCol<nRows; ++iCol)
                    {
                        mat(iCurrRow+iRow, iCol) = std::atof(cntColTokens[iCol].c_str());
                    }
                    ++iCurrRow;
                }
            }
            else
            {
                int nEq = str.countAppearance('=');
                if (nEq != 0)
                {
                    status = "ERROR! Duplicated '=' in matrix definition";
                    return false;
                }
                
                if (matrices[iMatrix].getNoOfRows() == 0)
                {
                    status = "ERROR! Matrix should have specified number of rows at first entry";
                    return false;
                }

                auto sec3 = str.replace("]", " ");
                sec3.split(";", cntRowTokens);
                auto nRowsToProcess = cntRowTokens.size();
                if (iCurrRow < 0)
                    iCurrRow = 0;
                for (td::UINT4 iRow = 0; iRow < nRowsToProcess; ++iRow)
                {
                    if (iCurrRow > nRows)
                    {
                        status = "ERROR! Too many rows";
                        return false;
                    }
                    const auto& row = cntRowTokens[iRow];
                    row.split(" ,", cntColTokens);
                    td::UINT4 nCols = td::UINT4(cntColTokens.size());
                    if (nRows == 0)
                    {
                        nRows = nCols;
                        matrices[iMatrix].reserve(nRows, nRows, nullptr, true);
                    }
                    else if (nRows != nCols)
                    {
                        status = "ERROR! Number of rows and columns mismatch";
                        return false;
                    }
                    
                    auto mat = matrices[iMatrix].getManipulator();
                    for (td::UINT4 iCol=0; iCol<nRows; ++iCol)
                    {
                        mat(iCurrRow+iRow, iCol) = std::atof(cntColTokens[iCol].c_str());
                    }
                    ++iCurrRow;
                }
            }
        }

    }
    return true;
}

static void writeAccelorometerData(arch::MemoryOut& memDigitalOut, const td::String& fullOutFleName, const td::String& accelData, td::MutableString& mStr, gui::LineEdit& status)
{
    td::String fullAccDataFileName = fo::replaceFileExtension<false>(fullOutFleName, ".dacc");
    std::ofstream f;
    if (!fo::createTextFile(f, fullAccDataFileName))
    {
        status = "ERROR! Cannto create accelorometer data file!";
        return;
    }
    
    fo::writeString(f, accelData);
    f.close();
    
    td::String accFileName = fo::getFilename(fullAccDataFileName);
    
    memDigitalOut.put("\n\nDataSets:");
    memDigitalOut.put("\n\t// The 'conn' attribute (in case of txt or sqlite type) accepts:");
    memDigitalOut.put("\n\t//   - Full file path (doesn't start with $)");
    memDigitalOut.put("\n\t//   - $ModLoc - relative to model location");
    memDigitalOut.put("\n\t//   - $AppRes - relative to app resource folder");
    memDigitalOut.put("\n\t//   - $Home   - relative to user's home folder");
    memDigitalOut.put("\n\t// The 'conn' attribute, in case of ODBC type, contains full ODBC connection string");
    mStr.appendFormat("\n\tdsAccell [type=txt conn=\"$ModLoc/%s\" data=\"SELECT time, xAcc as ag\"]", accFileName.c_str());
    memDigitalOut.put(mStr.c_str(), mStr.length());
    mStr.reset();
//    memDigitalOut.put("\n\tdsAccell [type=txt conn=\"$ModLoc/EQ_Petrovac_01.dacc\" data=\"SELECT time, xAcc as ax, yAcc as ay\"]");
    memDigitalOut.put("\n");
    memDigitalOut.put("\nSamplers:");
    memDigitalOut.put("\n\taccell [ds=dsAccell param=\"time\" outVals=\"ag\"] -> a_g");
    memDigitalOut.put("\n");
    memDigitalOut.put("\nPreProc:");
    memDigitalOut.put("\n\tsample(accell, t)\n");
}

// ----- NOVI CreateModel --------
// status je td::String da bi funkcija bila bezbjedna za pozivanje iz
// pozadinskog (worker) thread-a (GUI kontrole ne smiju se dirati van
// glavnog thread-a). onProgress javlja napredak (0.0-1.0) u realnom
// vremenu dok konverzija radi u drugom thread-u.
bool createModel(const td::String& inputFileName, const td::String& outFileName,
                  sc::IPlugin* pIPlugin, const Options& options, td::String& status,
                  std::function<void(double)> onProgress)
{
    mu::ScopedCLocale scopedLocale;
    if (onProgress) onProgress(0.02);
    
    // Parsiraj MATPOWER fajl
    std::vector<Bus> buses;
    std::vector<Branch> branches;
    std::vector<Gen> gens;
    double baseMVA = 100.0;
    
    std::string inFile(inputFileName.c_str());
    if (!parseMATPOWER(inFile, buses, branches, gens, baseMVA))
    {
        status = "ERROR! Cannot parse MATPOWER file!";
        return false;
    }
    
    if (buses.empty() || branches.empty())
    {
        status = "ERROR! Empty network data!";
        return false;
    }
    if (onProgress) onProgress(0.15);
    
    // Izgradi Y matricu (natID dense::CmplxMatrix - obavezno po uputstvu)
    int nBus = (int)buses.size();
    dense::CmplxMatrix Y;
    buildYMatrix(buses, branches, nBus, Y);
    if (onProgress) onProgress(0.25);
    
    // SSSC parametri - dolaze iz GUI (Options tab), NIJE hardkodirano
    int ssscFrom = int(options._ssscFromBus);
    int ssscTo = int(options._ssscToBus);
    double X_sssc = double(options._ssscReactance);

    if (X_sssc <= 0.0)
    {
        status = "ERROR! SSSC reactance must be positive!";
        return false;
    }

    // Nadji indekse busa (bus._id nije nuzno == index u nizu)
    int idxFrom = -1, idxTo = -1;
    for (int i = 0; i < nBus; ++i)
    {
        if (buses[i]._id == ssscFrom) idxFrom = i;
        if (buses[i]._id == ssscTo)   idxTo = i;
    }
    if (idxFrom < 0 || idxTo < 0)
    {
        status = "ERROR! SSSC From/To bus ID does not exist in the network!";
        return false;
    }

    // Provjeri da grana ssscFrom-ssscTo STVARNO postoji u mrezi
    // (SSSC je serijski uredjaj - mora sjediti na postojecoj liniji)
    {
        auto yCheck = Y.getManipulator();
        if (std::abs(yCheck(idxFrom, idxTo)) < 1e-10)
        {
            status = "ERROR! No branch exists between SSSC From/To buses!";
            return false;
        }
    }
    
    // Generiši dmodl
    auto pDigitalModel = pIPlugin->getArchive(sc::IPlugin::ArchType::DigitalModel);
    auto& memOut = *pDigitalModel;
    
    double vSsscSet = double(options._ssscVoltage);
    double deltaSsscSet = double(options._ssscAngle);
    generateDmodl(buses, branches, gens, Y, baseMVA, ssscFrom, ssscTo, X_sssc,
                  vSsscSet, deltaSsscSet, memOut, onProgress);
    
    // Snimi fajl
    std::ofstream fOut;
    if (!fo::createTextFile(fOut, outFileName))
    {
        status = "ERROR! Cannot create output file!";
        return false;
    }
    memOut.writeToFile(fOut);
    fOut.close();
    if (onProgress) onProgress(1.0);
    
    status = "SUCCESS! Model created!";
    return true;
}






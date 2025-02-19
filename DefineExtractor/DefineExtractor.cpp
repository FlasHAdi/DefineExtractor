#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <regex>
#include <map>
#include <algorithm>
#include <unordered_set>
#include <chrono>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif
#include <limits>
#if __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#error "No filesystem support detected"
#endif

using namespace std;
using namespace std::chrono;

static std::mutex consoleMutex;

/***********************************************
 * FARBSUPPORT (Konsole)
 ***********************************************/
#ifdef _WIN32
void setColor(int color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}
#else
void setColor(int color) {
    // Basic color mapping: 7=default, 10=green, 12=red
    cout << "\033[" << color << "m";
}
#endif

/***********************************************
 * PROGRESS ANZEIGE
 ***********************************************/
void printProgress(size_t current, size_t total, int width = 50) {
    static auto lastUpdate = high_resolution_clock::now();
    auto now = high_resolution_clock::now();
    // Aktualisiere nicht zu oft
    if (duration_cast<milliseconds>(now - lastUpdate).count() < 100) {
        return;
    }
    lastUpdate = now;

    if (total == 0) return;
    float ratio = float(current) / float(total);
    int c = int(ratio * width);

    lock_guard<mutex> lock(consoleMutex);
    cout << "[";
    for (int i = 0; i < width; ++i) {
        if (i < c) cout << "#";
        else       cout << " ";
    }
    cout << "] " << (int)(ratio * 100.0f) << " %\r" << flush;
}

/***********************************************
 * DATA STRUCTURES
 ***********************************************/
struct CodeBlock {
    string filename;
    string content;
};

struct ParseResult {
    vector<CodeBlock> defineBlocks;
    vector<CodeBlock> functionBlocks;
};

/***********************************************
 * GLOBALE (oder statische) HELFER FÜR LINIENZÄHLUNG
 ***********************************************/
 // Cache für bereits ermittelte Zeilenanzahlen
static std::mutex lineCountCacheMutex;
static std::unordered_map<std::string, size_t> lineCountCache;

/**
 * Ermittelt einmalig die Anzahl Zeilen in filename.
 * Falls bereits im Cache vorhanden, wird der gespeicherte Wert zurückgegeben.
 */
size_t getFileLineCount(const string& filename)
{
    {
        // Zuerst schauen wir ohne Lock, aber trotzdem safe in C++17/20?
        // Zur Sicherheit oder bei älteren C++ Versionen: Lock benutzen.
        lock_guard<mutex> lk(lineCountCacheMutex);
        auto it = lineCountCache.find(filename);
        if (it != lineCountCache.end()) {
            return it->second;
        }
    }

    // Nicht gefunden: Datei einmalig öffnen und zählen
    ifstream ifs(filename);
    if (!ifs.is_open()) {
        // Datei nicht vorhanden?
        // Speichere 0 im Cache und return 0.
        lock_guard<mutex> lk(lineCountCacheMutex);
        lineCountCache[filename] = 0;
        return 0;
    }

    size_t count = 0;
    string tmp;
    while (getline(ifs, tmp)) {
        count++;
    }

    {
        lock_guard<mutex> lk(lineCountCacheMutex);
        lineCountCache[filename] = count;
    }

    return count;
}

/**
 * Summiert die Zeilenzahl aller Dateien, unter Verwendung des obigen Caches.
 */
size_t getTotalLineCount(const vector<string>& files)
{
    size_t total = 0;
    for (auto& f : files) {
        total += getFileLineCount(f);
    }
    return total;
}

/***********************************************
 * TEIL A: C++-Parsing (#if define, function-blocks)
 ***********************************************/
static const regex endifRegex(R"(^\s*#\s*endif\b)",
    regex_constants::ECMAScript | regex_constants::optimize);

static const regex anyIfStartRegex(R"(^\s*#\s*(if|ifdef|ifndef)\b)",
    regex_constants::ECMAScript | regex_constants::optimize);

regex createConditionalRegex(const string& define) {
    ostringstream pattern;
    pattern
        << R"((^\s*#(ifdef|ifndef)\s+)" << define << R"(\b))"
        << R"(|)"
        << R"((^\s*#(if|elif)\s+defined\s*\(\s*)" << define << R"(\s*\)))"
        << R"(|)"
        << R"((^\s*#(if|elif)\s+defined\s+)" << define << R"())"
        << R"(|)"
        << R"((^\s*#(if|elif)\s+\(?\s*)" << define << R"(\s*\)?))";
    return regex(pattern.str(), regex_constants::ECMAScript | regex_constants::optimize);
}

static const regex functionHeadRegex(
    R"(^\s*(?:inline\s+|static\s+|virtual\s+|constexpr\s+|friend\s+|typename\s+|[\w:\*&<>]+\s+)*[\w:\*&<>]+\s+\w[\w:\*&<>]*\s*\([^)]*\)\s*(\{|;|$))",
    regex_constants::ECMAScript | regex_constants::optimize
);

/**
 * Parst eine einzelne Datei (C++/Header) auf #if-define-Blöcke und Funktionsblöcke.
 * filename: Datei
 * startDefineRegex: spezieller Regex für das gewählte #define
 * processed: Atomarer Zähler für Fortschritt
 * totalLines: Gesamtanzahl Zeilen aller Dateien
 * outLineCount: wird hochgezählt um die Zahl der gelesenen Zeilen dieser Datei
 */
pair<vector<CodeBlock>, vector<CodeBlock>>
parseFileSinglePass(const string& filename,
    const regex& startDefineRegex,
    atomic<size_t>& processed,
    size_t totalLines,
    size_t& outLineCount)
{
    vector<CodeBlock> defineBlocks;
    vector<CodeBlock> functionBlocks;

    ifstream ifs(filename);
    if (!ifs.is_open()) {
        return make_pair(defineBlocks, functionBlocks);
    }

    bool insideDefineBlock = false;
    int  defineNesting = 0;
    ostringstream currentDefineBlock;

    bool inFunction = false;
    int braceCount = 0;
    bool functionRelevant = false;
    ostringstream currentFunc;

    bool potentialFunctionHead = false;
    ostringstream potentialHeadBuffer;

    string line;
    while (true) {
        if (!getline(ifs, line)) {
            break;
        }
        outLineCount++;

        size_t oldVal = processed.fetch_add(1, memory_order_relaxed);
        if ((oldVal + 1) % 200 == 0) {
            printProgress(oldVal + 1, totalLines);
        }

        // #if-block logic
        if (!insideDefineBlock) {
            if (regex_search(line, startDefineRegex)) {
                insideDefineBlock = true;
                defineNesting = 1;
                currentDefineBlock.str("");
                currentDefineBlock.clear();
                currentDefineBlock << line << "\n";
            }
        }
        else {
            currentDefineBlock << line << "\n";
            if (regex_search(line, anyIfStartRegex)) {
                defineNesting++;
            }
            else if (regex_search(line, endifRegex)) {
                defineNesting--;
                if (defineNesting <= 0) {
                    CodeBlock cb;
                    cb.filename = filename;
                    cb.content = "##########\n" + filename + "\n##########\n" +
                        currentDefineBlock.str();
                    defineBlocks.push_back(cb);
                    insideDefineBlock = false;
                    defineNesting = 0;
                    currentDefineBlock.str("");
                    currentDefineBlock.clear();
                }
            }
        }

        // Function-block logic
        bool lineHasBraceOrParen = (line.find('{') != string::npos ||
            line.find('}') != string::npos ||
            line.find('(') != string::npos);
        bool lineMatchesDefine = regex_search(line, startDefineRegex);

        if (!inFunction) {
            if (potentialFunctionHead) {
                potentialHeadBuffer << "\n" << line;
                bool hasOpenBrace = (line.find('{') != string::npos);
                bool hasSemicolon = (line.find(';') != string::npos);

                if (hasOpenBrace) {
                    inFunction = true;
                    braceCount = 0;
                    functionRelevant = false;
                    currentFunc.str("");
                    currentFunc.clear();

                    currentFunc << potentialHeadBuffer.str() << "\n";
                    for (char c : line) {
                        if (c == '{') braceCount++;
                        if (c == '}') braceCount--;
                    }
                    if (lineMatchesDefine) functionRelevant = true;
                    potentialFunctionHead = false;
                    potentialHeadBuffer.str("");
                    potentialHeadBuffer.clear();
                }
                else if (hasSemicolon) {
                    potentialFunctionHead = false;
                    potentialHeadBuffer.str("");
                    potentialHeadBuffer.clear();
                }
            }
            else {
                smatch match;
                if (regex_search(line, match, functionHeadRegex)) {
                    string trailingSymbol = match[1].str();
                    if (trailingSymbol == "{") {
                        inFunction = true;
                        braceCount = 0;
                        functionRelevant = false;
                        currentFunc.str("");
                        currentFunc.clear();
                        currentFunc << line << "\n";

                        for (char c : line) {
                            if (c == '{') braceCount++;
                            if (c == '}') braceCount--;
                        }
                        if (lineMatchesDefine) {
                            functionRelevant = true;
                        }
                    }
                    else if (trailingSymbol == ";") {
                        // forward declaration -> ignore
                    }
                    else {
                        // Möglicherweise Zeilenumbruch in der Funktionskopfdefinition
                        potentialFunctionHead = true;
                        potentialHeadBuffer.str("");
                        potentialHeadBuffer.clear();
                        potentialHeadBuffer << line;
                    }
                }
            }
        }
        else {
            // inFunction
            currentFunc << line << "\n";
            if (lineMatchesDefine) {
                functionRelevant = true;
            }
            if (lineHasBraceOrParen) {
                for (char c : line) {
                    if (c == '{') braceCount++;
                    if (c == '}') braceCount--;
                }
            }
            if (braceCount <= 0) {
                // Funktionsblock zu Ende
                if (functionRelevant) {
                    CodeBlock cb;
                    cb.filename = filename;
                    cb.content = "##########\n" + filename + "\n##########\n" +
                        currentFunc.str();
                    functionBlocks.push_back(cb);
                }
                inFunction = false;
                braceCount = 0;
                currentFunc.str("");
                currentFunc.clear();
                functionRelevant = false;
            }
        }
    }
    return make_pair(defineBlocks, functionBlocks);
}

/***********************************************
 * TEIL B: PYTHON-Parsen
 ***********************************************/
static const regex pythonIfAppRegex(
    R"((?:if|elif)\s*\(?\s*app\.(\w+))",
    regex_constants::ECMAScript | regex_constants::optimize);

static const regex defRegex(R"(^\s*def\s+[\w_]+)");

// naive Indentation-Hilfsfunktion
int getIndent(const string& ln) {
    int count = 0;
    for (char c : ln) {
        if (c == ' ') count++;
        else if (c == '\t') count += 4; // naive tab=4
        else break;
    }
    return count;
}

/**
 * Parst eine einzelne Python-Datei zeilenweise (ohne alles vorher einzulesen).
 * Sucht nach `if app.<param>` Blöcken + Funktionen (`def`) und ermittelt Funktions-Relevanz.
 */
pair<vector<CodeBlock>, vector<CodeBlock>>
parsePythonFileSinglePass(const string& filename,
    const string& param,
    atomic<size_t>& processed,
    size_t totalLines,
    size_t& outLineCount)
{
    vector<CodeBlock> ifBlocks;
    vector<CodeBlock> funcBlocks;

    ifstream ifs(filename);
    if (!ifs.is_open()) {
        return make_pair(ifBlocks, funcBlocks);
    }

    // build param-regex
    ostringstream oss;
    oss << R"((?:if|elif)\s*\(?\s*app\.)" << param << R"(\b)";
    regex ifParamRegex(oss.str(), regex_constants::ECMAScript | regex_constants::optimize);

    bool insideFunc = false;
    int funcIndent = 0;
    bool functionRelevant = false;
    ostringstream currentFunc;

    string line;
    while (true) {
        streampos currentPos = ifs.tellg();
        if (!getline(ifs, line)) {
            break;
        }
        outLineCount++;

        size_t oldVal = processed.fetch_add(1, memory_order_relaxed);
        if ((oldVal + 1) % 200 == 0) {
            printProgress(oldVal + 1, totalLines);
        }

        // Prüfe Funktions-Start
        if (regex_search(line, defRegex)) {
            // Schließe ggf. vorherige Funktion ab
            if (insideFunc && functionRelevant) {
                CodeBlock cb;
                cb.filename = filename;
                cb.content = "##########\n" + filename + "\n##########\n" +
                    currentFunc.str();
                funcBlocks.push_back(cb);
            }
            // Neue Funktion beginnt
            insideFunc = true;
            funcIndent = getIndent(line);
            functionRelevant = false;
            currentFunc.str("");
            currentFunc.clear();
            currentFunc << line << "\n";
            continue;
        }

        // Prüfe, ob wir die aktuelle Funktion verlassen
        // (wenn eine Zeile <= funcIndent + nicht leer + kein neuer 'def')
        if (insideFunc) {
            int currentIndent = getIndent(line);
            bool nextDef = regex_search(line, defRegex);
            if (!line.empty() && currentIndent <= funcIndent && !nextDef) {
                // Funktion endet
                if (functionRelevant) {
                    CodeBlock cb;
                    cb.filename = filename;
                    cb.content = "##########\n" + filename + "\n##########\n" +
                        currentFunc.str();
                    funcBlocks.push_back(cb);
                }
                insideFunc = false;
                currentFunc.str("");
                currentFunc.clear();
                functionRelevant = false;
            }
            else {
                // Wir sind weiter in der Funktion
                currentFunc << line << "\n";
            }
        }

        // Prüfe, ob Zeile "if app.param" enthält
        if (regex_search(line, ifParamRegex)) {
            int ifIndent = getIndent(line);

            // Block-Inhalt sammeln
            ostringstream blockContent;
            blockContent << line << "\n";

            // Lese nachfolgende Zeilen, solange Einrückung > ifIndent
            while (true) {
                streampos pos = ifs.tellg();
                string nextLine;
                if (!getline(ifs, nextLine)) {
                    break; // Ende Datei
                }
                outLineCount++;

                size_t oldVal2 = processed.fetch_add(1, memory_order_relaxed);
                if ((oldVal2 + 1) % 200 == 0) {
                    printProgress(oldVal2 + 1, totalLines);
                }

                int indentJ = getIndent(nextLine);
                if (!nextLine.empty() && indentJ <= ifIndent) {
                    // Wir gehen eine Zeile zurück
                    ifs.seekg(pos);
                    break;
                }
                blockContent << nextLine << "\n";
            }

            // Fertigen If-Block speichern
            CodeBlock cb;
            cb.filename = filename;
            cb.content = "##########\n" + filename + "\n##########\n" +
                blockContent.str();
            ifBlocks.push_back(cb);

            // Falls wir in einer Funktion sind, wird diese relevant
            if (insideFunc) {
                functionRelevant = true;
            }
        }
    }

    // Falls am Dateiende noch eine Funktion offen ist
    if (insideFunc && functionRelevant) {
        CodeBlock cb;
        cb.filename = filename;
        cb.content = "##########\n" + filename + "\n##########\n" +
            currentFunc.str();
        funcBlocks.push_back(cb);
    }

    return make_pair(ifBlocks, funcBlocks);
}

/***********************************************
 * SAMMEL-FUNKTION: alle python-Dateien parsen
 ***********************************************/
unordered_set<string> collectPythonParameters(const vector<string>& pyFiles) {
    unordered_set<string> params;
    for (auto& f : pyFiles) {
        ifstream ifs(f);
        if (!ifs.is_open()) continue;
        string line;
        while (getline(ifs, line)) {
            smatch m;
            if (regex_search(line, m, pythonIfAppRegex)) {
                if (m.size() > 1) {
                    params.insert(m[1].str());
                }
            }
        }
    }
    return params;
}

/***********************************************
 * WORKER-FUNKTIONEN für Multi-Threading (C++)
 ***********************************************/
static atomic<size_t> nextFileIndex{ 0 }; // Für dynamisches Scheduling

void parseWorkerDynamic(const vector<string>& files,
    const regex& startDefineRegex,
    atomic<size_t>& processed,
    size_t totalLines,
    vector<CodeBlock>& defineBlocksOut,
    vector<CodeBlock>& functionBlocksOut)
{
    // Jeder Thread sammelt lokal sein Ergebnis
    vector<CodeBlock> localDefine;
    vector<CodeBlock> localFunc;

    while (true) {
        size_t idx = nextFileIndex.fetch_add(1, memory_order_relaxed);
        if (idx >= files.size()) {
            break; // Keine Dateien mehr
        }
        const auto& filename = files[idx];

        size_t lineCountThisFile = 0;
        auto pr = parseFileSinglePass(filename, startDefineRegex, processed,
            totalLines, lineCountThisFile);

        // In lokalen Vektor anhängen
        localDefine.insert(localDefine.end(), pr.first.begin(), pr.first.end());
        localFunc.insert(localFunc.end(), pr.second.begin(), pr.second.end());
        printProgress(processed.load(), totalLines);
    }

    // Thread-sicher die Ergebnisse in die Ausgabe-Vektoren packen
    {
        lock_guard<mutex> lock(consoleMutex);
        defineBlocksOut.insert(defineBlocksOut.end(), localDefine.begin(), localDefine.end());
        functionBlocksOut.insert(functionBlocksOut.end(), localFunc.begin(), localFunc.end());
    }
}

/**
 * Parst alle C++-Dateien multithreaded, dynamische Lastverteilung.
 */
pair<vector<CodeBlock>, vector<CodeBlock>>
parseAllFilesMultiThread(const vector<string>& files, const string& define)
{
    // Erzeuge Regex für definierten Parameter
    regex startDefineRegex = createConditionalRegex(define);

    // Gesamte Zeilenzahl ermitteln (jetzt über Cache)
    cout << "Zaehle Zeilen (ohne doppelte Dateizugriffe) ...\n";
    size_t totalLines = getTotalLineCount(files);
    cout << "Gesamt: " << totalLines << " Zeilen.\n";

    // Multi-Threading Setup
    unsigned int hwThreads = thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 2;
    size_t numThreads = std::min<size_t>(hwThreads, files.size());
    cout << "Starte " << numThreads << " Thread(s)...\n";

    atomic<size_t> processed{ 0 };
    // Ergebnis-Speicher (thread-sicherer Zugriff in parseWorkerDynamic)
    vector<CodeBlock> allDefineBlocks;
    vector<CodeBlock> allFunctionBlocks;

    // Index zurücksetzen und Threads starten
    nextFileIndex.store(0);

    vector<thread> threads;
    auto startTime = high_resolution_clock::now();
    for (size_t t = 0; t < numThreads; ++t) {
        threads.emplace_back(parseWorkerDynamic, cref(files),
            cref(startDefineRegex),
            ref(processed),
            totalLines,
            ref(allDefineBlocks),
            ref(allFunctionBlocks));
    }
    for (auto& th : threads) {
        th.join();
    }

    printProgress(totalLines, totalLines);
    cout << "\n";

    auto endTime = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(endTime - startTime).count();
    cout << "Parsing '" << define << "' fertig in " << ms << " ms\n";

    return make_pair(allDefineBlocks, allFunctionBlocks);
}

/***********************************************
 * WORKER-FUNKTIONEN für Multi-Threading (Python)
 ***********************************************/
static atomic<size_t> nextPyFileIndex{ 0 };

void parsePythonWorkerDynamic(const vector<string>& files,
    const string& param,
    atomic<size_t>& processed,
    size_t totalLines,
    vector<CodeBlock>& ifBlocksOut,
    vector<CodeBlock>& funcBlocksOut)
{
    vector<CodeBlock> localIf;
    vector<CodeBlock> localFunc;

    while (true) {
        size_t idx = nextPyFileIndex.fetch_add(1, memory_order_relaxed);
        if (idx >= files.size()) {
            break; // Keine Dateien mehr
        }
        const auto& filename = files[idx];

        size_t lineCountThisFile = 0;
        auto pr = parsePythonFileSinglePass(filename, param, processed,
            totalLines, lineCountThisFile);

        localIf.insert(localIf.end(), pr.first.begin(), pr.first.end());
        localFunc.insert(localFunc.end(), pr.second.begin(), pr.second.end());
        printProgress(processed.load(), totalLines);
    }

    lock_guard<mutex> lock(consoleMutex);
    ifBlocksOut.insert(ifBlocksOut.end(), localIf.begin(), localIf.end());
    funcBlocksOut.insert(funcBlocksOut.end(), localFunc.begin(), localFunc.end());
}

pair<vector<CodeBlock>, vector<CodeBlock>>
parsePythonAllFilesMultiThread(const vector<string>& pyFiles, const string& param)
{
    // Zähle Zeilen (jetzt aus Cache)
    size_t totalLines = getTotalLineCount(pyFiles);
    cout << "Gesamt: " << totalLines << " Python-Zeilen.\n";

    unsigned int hwThreads = thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 2;
    size_t numThreads = std::min<size_t>(hwThreads, pyFiles.size());
    cout << "Starte " << numThreads << " Thread(s) (Python)...\n";

    atomic<size_t> processed{ 0 };
    vector<CodeBlock> allIfBlocks;
    vector<CodeBlock> allFuncBlocks;

    // Index zurücksetzen und Threads starten
    nextPyFileIndex.store(0);

    vector<thread> threads;
    auto startTime = high_resolution_clock::now();
    for (size_t t = 0; t < numThreads; ++t) {
        threads.emplace_back(parsePythonWorkerDynamic,
            cref(pyFiles),
            cref(param),
            ref(processed),
            totalLines,
            ref(allIfBlocks),
            ref(allFuncBlocks));
    }
    for (auto& th : threads) {
        th.join();
    }

    printProgress(totalLines, totalLines);
    cout << "\n";

    auto endTime = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(endTime - startTime).count();
    cout << "Parsing (app." << param << ") fertig in " << ms << " ms\n";

    return make_pair(allIfBlocks, allFuncBlocks);
}

/***********************************************
 * Hilfsfunktionen
 ***********************************************/
vector<string> findSourceFiles() {
    vector<string> result;
    for (auto& p : fs::recursive_directory_iterator(".")) {
        if (!fs::is_regular_file(p)) continue;
        auto ext = p.path().extension().string();
        if (ext == ".cpp" || ext == ".h") {
            result.push_back(p.path().string());
        }
    }
    return result;
}

vector<string> readDefines(const string& filename) {
    vector<string> result;
    ifstream ifs(filename);
    if (!ifs.is_open()) {
        cerr << "Konnte " << filename << " nicht öffnen!\n";
        return result;
    }

    static const regex defineRegex(R"(^\s*#\s*define\s+(\w+))");
    string line;
    while (getline(ifs, line)) {
        smatch m;
        if (regex_search(line, m, defineRegex)) {
            result.push_back(m[1].str());
        }
    }
    return result;
}

/***********************************************
 * MAIN + MENÜ
 ***********************************************/
int main() {
    fs::path startPath = fs::current_path();

    bool hasClientHeader = false;
    bool hasServerHeader = false;

    std::string clientHeaderName;
    std::string serverHeaderName;

    // mögliche python-root Verzeichnisse
    vector<string> possiblePythonRoots;

    // Rekursive Suche
    for (auto& p : fs::recursive_directory_iterator(startPath)) {
        if (fs::is_regular_file(p)) {
            std::string fn = p.path().filename().string();
            std::string lower = fn;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

            if (lower == "locale_inc.h") {
                hasClientHeader = true;
                clientHeaderName = p.path().string();
            }
            if (lower == "service.h" || lower == "commondefines.h") {
                hasServerHeader = true;
                serverHeaderName = p.path().string();
            }
        }
        // Prüfe root-Ordner
        if (fs::is_directory(p) && p.path().filename() == "root") {
            possiblePythonRoots.push_back(p.path().string());
        }
    }

    // Python-Root wählen (falls mehrere)
    bool hasPythonRoot = !possiblePythonRoots.empty();
    std::string pythonRoot;
    if (hasPythonRoot) {
        if (possiblePythonRoots.size() == 1) {
            pythonRoot = possiblePythonRoots[0];
        }
        else {
            cout << "Es wurden mehrere 'root'-Verzeichnisse gefunden:\n";
            for (size_t i = 0; i < possiblePythonRoots.size(); ++i) {
                cout << (i + 1) << ". " << possiblePythonRoots[i] << "\n";
            }
            cout << "\nBitte wählen Sie eines aus (1-" << possiblePythonRoots.size() << "): ";
            int selection = 0;
            cin >> selection;
            while (!cin || selection < 1 || selection >(int)possiblePythonRoots.size()) {
                cin.clear();
                cin.ignore(10000, '\n');
                cout << "Ungültige Eingabe! Bitte erneut wählen (1-"
                    << possiblePythonRoots.size() << "): ";
                cin >> selection;
            }
            pythonRoot = possiblePythonRoots[selection - 1];
        }
    }

    while (true) {
        cout << "\n================================\n"
            << "    H A U P T -  M E N U E     \n"
            << "================================\n";

        // 1. Client
        setColor(hasClientHeader ? 10 : 12);
        cout << "1. Client\n";
        // 2. Server
        setColor(hasServerHeader ? 10 : 12);
        cout << "2. Server\n";
        // 3. Python
        setColor(hasPythonRoot ? 10 : 12);
        cout << "3. Python\n";
        // 0. Exit
        setColor(7);
        cout << "0. Exit\n";
        cout << "Choice: ";
        int choice;
        cin >> choice;
        if (!cin || choice == 0) {
            break;
        }

        // A) Client
        if (choice == 1) {
            if (!hasClientHeader) {
                cerr << "Kein Client-Header (locale_inc.h) gefunden!\n";
                continue;
            }
            // lies #defines
            auto defines = readDefines(clientHeaderName);
            if (defines.empty()) {
                cerr << "Keine #define-Eintraege in " << clientHeaderName << "!\n";
                continue;
            }
            // suche c++ dateien
            auto sourceFiles = findSourceFiles();
            if (sourceFiles.empty()) {
                cerr << "Keine .cpp/.h-Dateien gefunden.\n";
                continue;
            }

            while (true) {
                cout << "\nCLIENT-Header: " << clientHeaderName << "\n";
                cout << "Gefundene #defines:\n";
                for (size_t i = 0; i < defines.size(); ++i) {
                    cout << (i + 1) << ". " << defines[i] << "\n";
                }
                cout << "0. Zurueck\nWahl: ";
                int dchoice;
                cin >> dchoice;
                if (!cin || dchoice == 0) {
                    break;
                }
                if (dchoice < 0 || dchoice >(int)defines.size()) {
                    cerr << "Ungueltige Auswahl\n";
                    continue;
                }
                string def = defines[dchoice - 1];
                auto results = parseAllFilesMultiThread(sourceFiles, def);
                auto& allDefineBlocks = results.first;
                auto& allFunctionBlocks = results.second;

                fs::create_directory("Output");
                // Write output
                {
                    ostringstream fname;
                    fname << "Output/CLIENT_" << def << "_DEFINE.txt";
                    ofstream outDef(fname.str());
                    unordered_set<string> defFiles;
                    for (auto& b : allDefineBlocks) {
                        outDef << b.content << "\n";
                        defFiles.insert(b.filename);
                    }
                    outDef << "\n--- SUMMARY (" << allDefineBlocks.size()
                        << " DEFINE-Block/Bloecke) in Dateien: ---\n";
                    for (auto& fn : defFiles) {
                        outDef << fn << "\n";
                    }
                }
                {
                    ostringstream fname;
                    fname << "Output/CLIENT_" << def << "_FUNC.txt";
                    ofstream outFunc(fname.str());
                    unordered_set<string> funcFiles;
                    for (auto& b : allFunctionBlocks) {
                        outFunc << b.content << "\n";
                        funcFiles.insert(b.filename);
                    }
                    outFunc << "\n--- SUMMARY (" << allFunctionBlocks.size()
                        << " Funktions-Block/Bloecke) in Dateien: ---\n";
                    for (auto& fn : funcFiles) {
                        outFunc << fn << "\n";
                    }
                }
                setColor(10);
                cout << "Fertig fuer define '" << def << "'\n";
                setColor(7);
            }
        }
        // B) Server
        else if (choice == 2) {
            if (!hasServerHeader) {
                cerr << "Kein Server-Header (service.h/commondefines.h) gefunden!\n";
                continue;
            }
            auto defines = readDefines(serverHeaderName);
            if (defines.empty()) {
                cerr << "Keine #define-Eintraege in " << serverHeaderName << "!\n";
                continue;
            }
            auto sourceFiles = findSourceFiles();
            if (sourceFiles.empty()) {
                cerr << "Keine .cpp/.h-Dateien gefunden.\n";
                continue;
            }
            while (true) {
                cout << "\nSERVER-Header: " << serverHeaderName << "\n";
                cout << "Gefundene #defines:\n";
                for (size_t i = 0; i < defines.size(); ++i) {
                    cout << (i + 1) << ". " << defines[i] << "\n";
                }
                cout << "0. Zurueck\nWahl: ";
                int dchoice;
                cin >> dchoice;
                if (!cin || dchoice == 0) {
                    break;
                }
                if (dchoice < 0 || dchoice >(int)defines.size()) {
                    cerr << "Ungueltige Auswahl\n";
                    continue;
                }
                string def = defines[dchoice - 1];
                auto results = parseAllFilesMultiThread(sourceFiles, def);
                auto& allDefineBlocks = results.first;
                auto& allFunctionBlocks = results.second;

                fs::create_directory("Output");
                {
                    ostringstream fname;
                    fname << "Output/SERVER_" << def << "_DEFINE.txt";
                    ofstream outDef(fname.str());
                    unordered_set<string> defFiles;
                    for (auto& b : allDefineBlocks) {
                        outDef << b.content << "\n";
                        defFiles.insert(b.filename);
                    }
                    outDef << "\n--- SUMMARY (" << allDefineBlocks.size()
                        << " DEFINE-Block/Bloecke) in Dateien: ---\n";
                    for (auto& fn : defFiles) {
                        outDef << fn << "\n";
                    }
                }
                {
                    ostringstream fname;
                    fname << "Output/SERVER_" << def << "_FUNC.txt";
                    ofstream outFunc(fname.str());
                    unordered_set<string> funcFiles;
                    for (auto& b : allFunctionBlocks) {
                        outFunc << b.content << "\n";
                        funcFiles.insert(b.filename);
                    }
                    outFunc << "\n--- SUMMARY (" << allFunctionBlocks.size()
                        << " Funktions-Block/Bloecke) in Dateien: ---\n";
                    for (auto& fn : funcFiles) {
                        outFunc << fn << "\n";
                    }
                }
                setColor(10);
                cout << "Fertig fuer define '" << def << "'\n";
                setColor(7);
            }
        }
        // C) Python
        else if (choice == 3) {
            if (!hasPythonRoot) {
                cerr << "python_root Ordner nicht gefunden!\n";
                continue;
            }

            // Ermittle alle .py in pythonRoot (rekursiv)
            vector<string> pyFiles;
            for (auto& p : fs::recursive_directory_iterator(pythonRoot)) {
                if (!fs::is_regular_file(p)) continue;
                if (p.path().extension() == ".py") {
                    pyFiles.push_back(p.path().string());
                }
            }
            if (pyFiles.empty()) {
                cerr << "Keine .py-Dateien in " << pythonRoot << "\n";
                continue;
            }
            // Sammle Parameter
            auto paramSet = collectPythonParameters(pyFiles);
            if (paramSet.empty()) {
                cerr << "Keine 'if app.xyz' in python_root gefunden!\n";
                continue;
            }
            vector<string> params(paramSet.begin(), paramSet.end());
            sort(params.begin(), params.end());

            while (true) {
                cout << "\nPython-Parameter im Ordner " << pythonRoot << ":\n";
                for (size_t i = 0; i < params.size(); ++i) {
                    cout << (i + 1) << ". " << params[i] << "\n";
                }
                cout << "0. Zurueck\nWahl: ";
                int pchoice;
                cin >> pchoice;
                if (!cin || pchoice == 0) {
                    break;
                }
                if (pchoice < 0 || pchoice >(int)params.size()) {
                    cerr << "Ungueltige Auswahl\n";
                    continue;
                }
                string chosenParam = params[pchoice - 1];

                // parse + ausgabe
                auto pyResults = parsePythonAllFilesMultiThread(pyFiles, chosenParam);
                auto& ifBlocks = pyResults.first;
                auto& funcBlocks = pyResults.second;

                fs::create_directory("Output");
                // "If-Blöcke" analog "DEFINE"
                {
                    ostringstream fname;
                    fname << "Output/PYTHON_" << chosenParam << "_DEFINE.txt";
                    ofstream out(fname.str());
                    unordered_set<string> defFiles;
                    for (auto& b : ifBlocks) {
                        out << b.content << "\n";
                        defFiles.insert(b.filename);
                    }
                    out << "\n--- SUMMARY (" << ifBlocks.size()
                        << " If-Block/Bloecke) in Dateien: ---\n";
                    for (auto& fn : defFiles) {
                        out << fn << "\n";
                    }
                }
                // "Funktions-Blöcke"
                {
                    ostringstream fname;
                    fname << "Output/PYTHON_" << chosenParam << "_FUNC.txt";
                    ofstream out(fname.str());
                    unordered_set<string> funcFiles;
                    for (auto& b : funcBlocks) {
                        out << b.content << "\n";
                        funcFiles.insert(b.filename);
                    }
                    out << "\n--- SUMMARY (" << funcBlocks.size()
                        << " Funktions-Block/Bloecke) in Dateien: ---\n";
                    for (auto& fn : funcFiles) {
                        out << fn << "\n";
                    }
                }
                setColor(10);
                cout << "Fertig fuer app." << chosenParam << "\n";
                setColor(7);
            }
        }
        else {
            cerr << "Unbekannte Auswahl!\n";
        }
    }

    return 0;
}

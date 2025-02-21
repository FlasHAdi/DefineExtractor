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

using namespace std::chrono;

static std::mutex consoleMutex;

/***********************************************
 * FARBSUPPORT (Konsole)
 ***********************************************/

 /**
  * Setzt die Textfarbe in der Konsole.
  * @param color Farbcode. Unter Windows entsprechend der WinAPI-Farbwerte,
  *              unter Unix wird ein ANSI-Escape-Code ausgegeben.
  */
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

 /**
  * Zeigt einen Fortschrittsbalken (Progress-Bar) in der Konsole an.
  * @param current Aktueller Fortschritt (z.B. Anzahl bearbeiteter Zeilen).
  * @param total Gesamtmenge (z.B. Gesamtanzahl Zeilen).
  * @param width Breite des Fortschrittsbalkens in Zeichen.
  */
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

    std::lock_guard<std::mutex> lock(consoleMutex);
    std::cout << "[";
    for (int i = 0; i < width; ++i) {
        if (i < c) std::cout << "#";
        else       std::cout << " ";
    }
    std::cout << "] " << (int)(ratio * 100.0f) << " %\r" << std::flush;
}

/***********************************************
 * DATA STRUCTURES
 ***********************************************/
struct CodeBlock {
    std::string filename;
    std::string content;
};

struct ParseResult {
    std::vector<CodeBlock> defineBlocks;
    std::vector<CodeBlock> functionBlocks;
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
 * @param filename Name der Datei, deren Zeilenanzahl ermittelt werden soll.
 * @return Anzahl der Zeilen in der Datei (0 falls Datei nicht vorhanden).
 */
size_t getFileLineCount(const std::string& filename)
{
    {
        std::lock_guard<std::mutex> lk(lineCountCacheMutex);
        auto it = lineCountCache.find(filename);
        if (it != lineCountCache.end()) {
            return it->second;
        }
    }

    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
        // Datei nicht vorhanden?
        std::lock_guard<std::mutex> lk(lineCountCacheMutex);
        lineCountCache[filename] = 0;
        return 0;
    }

    size_t count = 0;
    std::string tmp;
    while (getline(ifs, tmp)) {
        count++;
    }

    {
        std::lock_guard<std::mutex> lk(lineCountCacheMutex);
        lineCountCache[filename] = count;
    }

    return count;
}

/**
 * Summiert die Zeilenzahl aller Dateien, unter Verwendung des obigen Caches.
 * @param files Liste von Dateinamen.
 * @return Gesamtanzahl Zeilen über alle Dateien.
 */
size_t getTotalLineCount(const std::vector<std::string>& files)
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
static const std::regex endifRegex(R"(^\s*#\s*endif\b)",
    std::regex_constants::ECMAScript | std::regex_constants::optimize);

static const std::regex anyIfStartRegex(R"(^\s*#\s*(if|ifdef|ifndef)\b)",
    std::regex_constants::ECMAScript | std::regex_constants::optimize);

/**
 * Erzeugt einen Regex-Ausdruck, der verschiedene Varianten von if-defines
 * für den gewünschten Parameter (z.B. #ifdef DEFINE, #if defined(DEFINE) etc.)
 * abdeckt.
 * @param define Der zu suchende define-Name.
 * @return Ein std::regex, der die entsprechenden Zeilen erkennt.
 */
std::regex createConditionalRegex(const std::string& define) {
    std::ostringstream pattern;
    pattern
        << R"((^\s*#(ifdef|ifndef)\s+)" << define << R"(\b))"
        << R"(|)"
        << R"((^\s*#(if|elif)\s+defined\s*\(\s*)" << define << R"(\s*\)))"
        << R"(|)"
        << R"((^\s*#(if|elif)\s+defined\s+)" << define << R"())"
        << R"(|)"
        << R"((^\s*#(if|elif)\s+\(?\s*)" << define << R"(\s*\)?))";
    return std::regex(pattern.str(), std::regex_constants::ECMAScript | std::regex_constants::optimize);
}

static const std::regex functionHeadRegex(
    R"(^\s*(?:inline\s+|static\s+|virtual\s+|constexpr\s+|friend\s+|typename\s+|[\w:\*&<>]+\s+)*[\w:\*&<>]+\s+\w[\w:\*&<>]*\s*\([^)]*\)\s*(\{|;|$))",
    std::regex_constants::ECMAScript | std::regex_constants::optimize
);

/**
 * Parst eine einzelne Datei (C++/Header) auf #if-define-Blöcke und Funktionsblöcke.
 * @param filename Datei, die untersucht wird.
 * @param startDefineRegex Spezieller Regex für das gewählte #define.
 * @param processed Atomarer Zähler für bereits gelesene Zeilen (Fortschritt).
 * @param totalLines Gesamtanzahl Zeilen aller zu untersuchenden Dateien.
 * @param outLineCount Gibt die Anzahl gelesener Zeilen für diese Datei zurück.
 * @return Ein Paar aus (Liste von CodeBlöcken mit #if-define, Liste von CodeBlöcken mit Funktionsblöcken).
 */
std::pair<std::vector<CodeBlock>, std::vector<CodeBlock>>
parseFileSinglePass(const std::string& filename,
    const std::regex& startDefineRegex,
    std::atomic<size_t>& processed,
    size_t totalLines,
    size_t& outLineCount)
{
    std::vector<CodeBlock> defineBlocks;
    std::vector<CodeBlock> functionBlocks;

    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
        return make_pair(defineBlocks, functionBlocks);
    }

    bool insideDefineBlock = false;
    int  defineNesting = 0;
    std::ostringstream currentDefineBlock;

    bool inFunction = false;
    int braceCount = 0;
    bool functionRelevant = false;
    std::ostringstream currentFunc;

    bool potentialFunctionHead = false;
    std::ostringstream potentialHeadBuffer;

    std::string line;
    while (true) {
        if (!getline(ifs, line)) {
            break;
        }
        outLineCount++;

        size_t oldVal = processed.fetch_add(1, std::memory_order_relaxed);
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
        bool lineHasBraceOrParen = (line.find('{') != std::string::npos ||
            line.find('}') != std::string::npos ||
            line.find('(') != std::string::npos);
        bool lineMatchesDefine = regex_search(line, startDefineRegex);

        if (!inFunction) {
            if (potentialFunctionHead) {
                potentialHeadBuffer << "\n" << line;
                bool hasOpenBrace = (line.find('{') != std::string::npos);
                bool hasSemicolon = (line.find(';') != std::string::npos);

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
                std::smatch match;
                if (regex_search(line, match, functionHeadRegex)) {
                    std::string trailingSymbol = match[1].str();
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
static const std::regex pythonIfAppRegex(
    R"((?:if|elif)\s*\(?\s*app\.(\w+))",
    std::regex_constants::ECMAScript | std::regex_constants::optimize);

static const std::regex defRegex(R"(^\s*def\s+[\w_]+)");

/**
 * Naive Hilfsfunktion zur Ermittlung der Einrückung (Indentation).
 * Wandelt Tabs in 4 Spaces um, was in der Praxis fehleranfällig sein kann.
 * @param ln Zu analysierende Zeile.
 * @return Geschätzte Einrückungs-Tiefe (Anzahl Spaces).
 */
int getIndent(const std::string& ln) {
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
 * Sucht nach `if app.<param>` Blöcken und Funktionen (`def`) und ermittelt,
 * ob eine Funktion durch einen solchen if-Block "relevant" wird.
 * @param filename Name der Python-Datei.
 * @param param Der Parameter (z.B. "xyz" in "if app.xyz").
 * @param processed Atomarer Zähler für Fortschritt (gelesene Zeilen).
 * @param totalLines Gesamtzahl Zeilen in allen .py-Dateien.
 * @param outLineCount Anzahl gelesener Zeilen dieser Datei.
 * @return Ein Paar: (Liste gefundener if app.-Blöcke, Liste relevanter Funktionsblöcke).
 */
std::pair<std::vector<CodeBlock>, std::vector<CodeBlock>>
parsePythonFileSinglePass(const std::string& filename,
    const std::string& param,
    std::atomic<size_t>& processed,
    size_t totalLines,
    size_t& outLineCount)
{
    std::vector<CodeBlock> ifBlocks;
    std::vector<CodeBlock> funcBlocks;

    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
        return make_pair(ifBlocks, funcBlocks);
    }

    // build param-regex
    std::ostringstream oss;
    oss << R"((?:if|elif)\s*\(?\s*app\.)" << param << R"(\b)";
    std::regex ifParamRegex(oss.str(), std::regex_constants::ECMAScript | std::regex_constants::optimize);

    bool insideFunc = false;
    int funcIndent = 0;
    bool functionRelevant = false;
    std::ostringstream currentFunc;

    std::string line;
    while (true) {
        std::streampos currentPos = ifs.tellg();
        if (!getline(ifs, line)) {
            break;
        }
        outLineCount++;

        size_t oldVal = processed.fetch_add(1, std::memory_order_relaxed);
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
            std::ostringstream blockContent;
            blockContent << line << "\n";

            // Lese nachfolgende Zeilen, solange Einrückung > ifIndent
            while (true) {
                std::streampos pos = ifs.tellg();
                std::string nextLine;
                if (!getline(ifs, nextLine)) {
                    break; // Ende Datei
                }
                outLineCount++;

                size_t oldVal2 = processed.fetch_add(1, std::memory_order_relaxed);
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

 /**
  * Durchsucht alle angegebenen Python-Dateien nach Zeilen wie `if app.<param>`
  * und extrahiert dabei alle auftretenden Parameter-Namen.
  * @param pyFiles Liste mit Pfaden zu Python-Dateien.
  * @return Menge aller gefundenen Parameter-Namen (z.B. {"xyz", "debug", ...}).
  */
std::unordered_set<std::string> collectPythonParameters(const std::vector<std::string>& pyFiles) {
    std::unordered_set<std::string> params;

    static const std::unordered_set<std::string> blacklist = {
        "loggined", "VK_UP", "VK_RIGHT", "VK_LEFT", "VK_HOME", "VK_END", "VK_DOWN", "VK_DELETE",
        "TARGET", "SELL", "BUY", "DIK_DOWN", "DIK_F1", "DIK_F2", "DIK_F3", "DIK_F4",
        "DIK_H", "DIK_LALT", "DIK_LCONTROL", "DIK_RETURN", "DIK_SYSRQ", "DIK_UP", "DIK_V", "GetGlobalTime",
        "GetTime", "IsDevStage", "IsEnableTestServerFlag", "IsExistFile", "IsPressed", "IsWebPageMode",
    };

    for (const auto& f : pyFiles) {
        std::ifstream ifs(f);
        if (!ifs.is_open()) continue;

        std::string line;
        while (getline(ifs, line)) {
            std::smatch m;
            if (std::regex_search(line, m, pythonIfAppRegex) && m.size() > 1) {
                std::string param = m[1].str();

                // Falls Parameter in der Blacklist ist, überspringen
                if (blacklist.find(param) == blacklist.end()) {
                    params.insert(param);
                }
            }
        }
    }
    return params;
}

/***********************************************
 * WORKER-FUNKTIONEN für Multi-Threading (C++)
 ***********************************************/

static std::atomic<size_t> nextFileIndex{ 0 }; // Für dynamisches Scheduling

/**
 * Worker-Funktion für den Multi-Threaded C++-Parser. Jeder Thread holt sich
 * mittels nextFileIndex eine Datei und parst diese.
 * @param files Liste aller zu parsenden Dateien.
 * @param startDefineRegex Regex, der auf den #define-Parameter abgestimmt ist.
 * @param processed Atomarer Zähler für gelesene Zeilen.
 * @param totalLines Gesamtanzahl Zeilen aller Dateien.
 * @param defineBlocksOut Ausgabe-Liste für gefundene #if-define-Blöcke.
 * @param functionBlocksOut Ausgabe-Liste für gefundene Funktionsblöcke.
 */
void parseWorkerDynamic(const std::vector<std::string>& files,
    const std::regex& startDefineRegex,
    std::atomic<size_t>& processed,
    size_t totalLines,
    std::vector<CodeBlock>& defineBlocksOut,
    std::vector<CodeBlock>& functionBlocksOut)
{
    // Jeder Thread sammelt lokal sein Ergebnis
    std::vector<CodeBlock> localDefine;
    std::vector<CodeBlock> localFunc;

    while (true) {
        size_t idx = nextFileIndex.fetch_add(1, std::memory_order_relaxed);
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
        std::lock_guard<std::mutex> lock(consoleMutex);
        defineBlocksOut.insert(defineBlocksOut.end(), localDefine.begin(), localDefine.end());
        functionBlocksOut.insert(functionBlocksOut.end(), localFunc.begin(), localFunc.end());
    }
}

/**
 * Parst alle C++-Dateien multithreaded mit dynamischer Lastverteilung.
 * @param files Liste aller C++/Header-Dateien.
 * @param define Zu suchender #define-Parameter.
 * @return Ein Paar aus (Liste #if-define-Blöcke, Liste Funktionsblöcke).
 */
std::pair<std::vector<CodeBlock>, std::vector<CodeBlock>>
parseAllFilesMultiThread(const std::vector<std::string>& files, const std::string& define)
{
    // Erzeuge Regex für definierten Parameter
    std::regex startDefineRegex = createConditionalRegex(define);

    // Gesamte Zeilenzahl ermitteln (jetzt über Cache)
    std::cout << "Zaehle Zeilen (ohne doppelte Dateizugriffe) ...\n";
    size_t totalLines = getTotalLineCount(files);
    std::cout << "Gesamt: " << totalLines << " Zeilen.\n";

    // Multi-Threading Setup
    unsigned int hwThreads = std::thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 2;
    size_t numThreads = std::min<size_t>(hwThreads, files.size());
    std::cout << "Starte " << numThreads << " Thread(s)...\n";

    std::atomic<size_t> processed{ 0 };
    // Ergebnis-Speicher (thread-sicherer Zugriff in parseWorkerDynamic)
    std::vector<CodeBlock> allDefineBlocks;
    std::vector<CodeBlock> allFunctionBlocks;

    // Index zurücksetzen und Threads starten
    nextFileIndex.store(0);

    std::vector<std::thread> threads;
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
    std::cout << "\n";

    auto endTime = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(endTime - startTime).count();
    std::cout << "Parsing '" << define << "' fertig in " << ms << " ms\n";

    return make_pair(allDefineBlocks, allFunctionBlocks);
}

/***********************************************
 * WORKER-FUNKTIONEN für Multi-Threading (Python)
 ***********************************************/

static std::atomic<size_t> nextPyFileIndex{ 0 };

/**
 * Worker-Funktion für den Multi-Threaded-Python-Parser. Jeder Thread holt sich
 * über nextPyFileIndex eine Python-Datei und parst diese.
 * @param files Liste aller Python-Dateien.
 * @param param Gesuchter Parameter, z.B. "debug" in `if app.debug`.
 * @param processed Atomarer Zähler für gelesene Zeilen.
 * @param totalLines Gesamtanzahl Zeilen aller Python-Dateien.
 * @param ifBlocksOut Ausgabe-Liste für gefundene `if app.param`-Blöcke.
 * @param funcBlocksOut Ausgabe-Liste für relevante Funktionsblöcke.
 */
void parsePythonWorkerDynamic(const std::vector<std::string>& files,
    const std::string& param,
    std::atomic<size_t>& processed,
    size_t totalLines,
    std::vector<CodeBlock>& ifBlocksOut,
    std::vector<CodeBlock>& funcBlocksOut)
{
    std::vector<CodeBlock> localIf;
    std::vector<CodeBlock> localFunc;

    while (true) {
        size_t idx = nextPyFileIndex.fetch_add(1, std::memory_order_relaxed);
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

    std::lock_guard<std::mutex> lock(consoleMutex);
    ifBlocksOut.insert(ifBlocksOut.end(), localIf.begin(), localIf.end());
    funcBlocksOut.insert(funcBlocksOut.end(), localFunc.begin(), localFunc.end());
}

/**
 * Parst alle angegebenen Python-Dateien parallel, sucht nach `if app.param` Blöcken
 * und erkennt Funktionen, in denen diese Blöcke enthalten sind.
 * @param pyFiles Liste aller Python-Dateien.
 * @param param Parameter (z.B. "debug"), der in `if app.debug` gesucht wird.
 * @return Ein Paar aus (Liste gefundener if-Blöcke, Liste relevanter Funktionsblöcke).
 */
std::pair<std::vector<CodeBlock>, std::vector<CodeBlock>>
parsePythonAllFilesMultiThread(const std::vector<std::string>& pyFiles, const std::string& param)
{
    // Zähle Zeilen (jetzt aus Cache)
    size_t totalLines = getTotalLineCount(pyFiles);
    std::cout << "Gesamt: " << totalLines << " Python-Zeilen.\n";

    unsigned int hwThreads = std::thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 2;
    size_t numThreads = std::min<size_t>(hwThreads, pyFiles.size());
    std::cout << "Starte " << numThreads << " Thread(s) (Python)...\n";

    std::atomic<size_t> processed{ 0 };
    std::vector<CodeBlock> allIfBlocks;
    std::vector<CodeBlock> allFuncBlocks;

    // Index zurücksetzen und Threads starten
    nextPyFileIndex.store(0);

    std::vector<std::thread> threads;
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
    std::cout << "\n";

    auto endTime = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(endTime - startTime).count();
    std::cout << "Parsing (app." << param << ") fertig in " << ms << " ms\n";

    return make_pair(allIfBlocks, allFuncBlocks);
}

/***********************************************
 * Hilfsfunktionen
 ***********************************************/

 /**
  * Durchsucht rekursiv das aktuelle Verzeichnis (".") nach Dateien
  * mit Endung .cpp oder .h.
  * @return Liste mit Pfaden aller gefundenen C++-Dateien.
  */
std::vector<std::string> findSourceFiles() {
    std::vector<std::string> result;
    for (auto& p : fs::recursive_directory_iterator(".")) {
        if (!fs::is_regular_file(p)) continue;
        auto ext = p.path().extension().string();
        if (ext == ".cpp" || ext == ".h") {
            result.push_back(p.path().string());
        }
    }
    return result;
}

/**
 * Liest Zeile für Zeile eine Datei ein und sucht nach `#define <TOKEN>`.
 * @param filename Name der Datei, z.B. ein Headerfile.
 * @return Liste aller gefundenen define-Namen.
 */
std::vector<std::string> readDefines(const std::string& filename) {
    std::vector<std::string> result;
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
        std::cerr << "Konnte " << filename << " nicht öffnen!\n";
        return result;
    }

    static const std::regex defineRegex(R"(^\s*#\s*define\s+(\w+))");
    std::string line;
    while (getline(ifs, line)) {
        std::smatch m;
        if (regex_search(line, m, defineRegex)) {
            result.push_back(m[1].str());
        }
    }
    return result;
}

/***********************************************
 * MAIN + MENÜ
 ***********************************************/

 /**
  * Hauptprogramm mit einfachem Konsolen-Menü für Client/Server/Python.
  * Sucht #defines in bestimmten Headern, sowie python_root-Verzeichnis.
  * Bietet dann die Auswahl an, einen define (C++/Header) oder einen Parameter
  * (Python) auszuwählen und führt den entsprechenden Parsing-Prozess aus.
  * Das Ergebnis wird in Output-Dateien geschrieben.
  * @return 0 bei regulärem Programmende.
  */
int main() {
    fs::path startPath = fs::current_path();

    bool hasClientHeader = false;
    bool hasServerHeader = false;

    std::string clientHeaderName;
    std::string serverHeaderName;

    // mögliche python-root Verzeichnisse
    std::vector<std::string> possiblePythonRoots;

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
            std::cout << "Es wurden mehrere 'root'-Verzeichnisse gefunden:\n";
            for (size_t i = 0; i < possiblePythonRoots.size(); ++i) {
                std::cout << (i + 1) << ". " << possiblePythonRoots[i] << "\n";
            }
            std::cout << "\nBitte wählen Sie eines aus (1-" << possiblePythonRoots.size() << "): ";
            int selection = 0;
            std::cin >> selection;
            while (!std::cin || selection < 1 || selection >(int)possiblePythonRoots.size()) {
                std::cin.clear();
                std::cin.ignore(10000, '\n');
                std::cout << "Ungültige Eingabe! Bitte erneut wählen (1-"
                    << possiblePythonRoots.size() << "): ";
                std::cin >> selection;
            }
            pythonRoot = possiblePythonRoots[selection - 1];
        }
    }

    while (true) {
        std::cout << "\n================================\n"
            << "    H A U P T -  M E N U E     \n"
            << "================================\n";

        // 1. Client
        setColor(hasClientHeader ? 10 : 12);
        std::cout << "1. Client\n";
        // 2. Server
        setColor(hasServerHeader ? 10 : 12);
        std::cout << "2. Server\n";
        // 3. Python
        setColor(hasPythonRoot ? 10 : 12);
        std::cout << "3. Python\n";
        // 0. Exit
        setColor(7);
        std::cout << "0. Exit\n";
        std::cout << "Choice: ";
        int choice;
        std::cin >> choice;
        if (!std::cin || choice == 0) {
            break;
        }

        // A) Client
        if (choice == 1) {
            if (!hasClientHeader) {
                std::cerr << "Kein Client-Header (locale_inc.h) gefunden!\n";
                continue;
            }
            // lies #defines
            auto defines = readDefines(clientHeaderName);
            if (defines.empty()) {
                std::cerr << "Keine #define-Eintraege in " << clientHeaderName << "!\n";
                continue;
            }
            // suche c++ dateien
            auto sourceFiles = findSourceFiles();
            if (sourceFiles.empty()) {
                std::cerr << "Keine .cpp/.h-Dateien gefunden.\n";
                continue;
            }

            while (true) {
                std::cout << "\nCLIENT-Header: " << clientHeaderName << "\n";
                std::cout << "Gefundene #defines:\n";
                for (size_t i = 0; i < defines.size(); ++i) {
                    std::cout << (i + 1) << ". " << defines[i] << "\n";
                }
                std::cout << "0. Zurueck\nWahl: ";
                int dchoice;
                std::cin >> dchoice;
                if (!std::cin || dchoice == 0) {
                    break;
                }
                if (dchoice < 0 || dchoice >(int)defines.size()) {
                    std::cerr << "Ungueltige Auswahl\n";
                    continue;
                }
                std::string def = defines[dchoice - 1];
                auto results = parseAllFilesMultiThread(sourceFiles, def);
                auto& allDefineBlocks = results.first;
                auto& allFunctionBlocks = results.second;

                fs::create_directory("Output");
                // Write output
                {
                    std::ostringstream fname;
                    fname << "Output/CLIENT_" << def << "_DEFINE.txt";
                    std::ofstream outDef(fname.str());
                    std::unordered_set<std::string> defFiles;
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
                    std::ostringstream fname;
                    fname << "Output/CLIENT_" << def << "_FUNC.txt";
                    std::ofstream outFunc(fname.str());
                    std::unordered_set<std::string> funcFiles;
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
                std::cout << "Fertig fuer define '" << def << "'\n";
                setColor(7);
            }
        }
        // B) Server
        else if (choice == 2) {
            if (!hasServerHeader) {
                std::cerr << "Kein Server-Header (service.h/commondefines.h) gefunden!\n";
                continue;
            }
            auto defines = readDefines(serverHeaderName);
            if (defines.empty()) {
                std::cerr << "Keine #define-Eintraege in " << serverHeaderName << "!\n";
                continue;
            }
            auto sourceFiles = findSourceFiles();
            if (sourceFiles.empty()) {
                std::cerr << "Keine .cpp/.h-Dateien gefunden.\n";
                continue;
            }
            while (true) {
                std::cout << "\nSERVER-Header: " << serverHeaderName << "\n";
                std::cout << "Gefundene #defines:\n";
                for (size_t i = 0; i < defines.size(); ++i) {
                    std::cout << (i + 1) << ". " << defines[i] << "\n";
                }
                std::cout << "0. Zurueck\nWahl: ";
                int dchoice;
                std::cin >> dchoice;
                if (!std::cin || dchoice == 0) {
                    break;
                }
                if (dchoice < 0 || dchoice >(int)defines.size()) {
                    std::cerr << "Ungueltige Auswahl\n";
                    continue;
                }
                std::string def = defines[dchoice - 1];
                auto results = parseAllFilesMultiThread(sourceFiles, def);
                auto& allDefineBlocks = results.first;
                auto& allFunctionBlocks = results.second;

                fs::create_directory("Output");
                {
                    std::ostringstream fname;
                    fname << "Output/SERVER_" << def << "_DEFINE.txt";
                    std::ofstream outDef(fname.str());
                    std::unordered_set<std::string> defFiles;
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
                    std::ostringstream fname;
                    fname << "Output/SERVER_" << def << "_FUNC.txt";
                    std::ofstream outFunc(fname.str());
                    std::unordered_set<std::string> funcFiles;
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
                std::cout << "Fertig fuer define '" << def << "'\n";
                setColor(7);
            }
        }
        // C) Python
        else if (choice == 3) {
            if (!hasPythonRoot) {
                std::cerr << "python_root Ordner nicht gefunden!\n";
                continue;
            }

            // Ermittle alle .py in pythonRoot (rekursiv)
            std::vector<std::string> pyFiles;
            for (auto& p : fs::recursive_directory_iterator(pythonRoot)) {
                if (!fs::is_regular_file(p)) continue;
                if (p.path().extension() == ".py") {
                    pyFiles.push_back(p.path().string());
                }
            }
            if (pyFiles.empty()) {
                std::cerr << "Keine .py-Dateien in " << pythonRoot << "\n";
                continue;
            }
            // Sammle Parameter
            auto paramSet = collectPythonParameters(pyFiles);
            if (paramSet.empty()) {
                std::cerr << "Keine 'if app.xyz' in python_root gefunden!\n";
                continue;
            }
            std::vector<std::string> params(paramSet.begin(), paramSet.end());
            sort(params.begin(), params.end());

            while (true) {
                std::cout << "\nPython-Parameter im Ordner " << pythonRoot << ":\n";
                for (size_t i = 0; i < params.size(); ++i) {
                    std::cout << (i + 1) << ". " << params[i] << "\n";
                }
                std::cout << "0. Zurueck\nWahl: ";
                int pchoice;
                std::cin >> pchoice;
                if (!std::cin || pchoice == 0) {
                    break;
                }
                if (pchoice < 0 || pchoice >(int)params.size()) {
                    std::cerr << "Ungueltige Auswahl\n";
                    continue;
                }
                std::string chosenParam = params[pchoice - 1];

                // parse + ausgabe
                auto pyResults = parsePythonAllFilesMultiThread(pyFiles, chosenParam);
                auto& ifBlocks = pyResults.first;
                auto& funcBlocks = pyResults.second;

                fs::create_directory("Output");
                // "If-Blöcke" analog "DEFINE"
                {
                    std::ostringstream fname;
                    fname << "Output/PYTHON_" << chosenParam << "_DEFINE.txt";
                    std::ofstream out(fname.str());
                    std::unordered_set<std::string> defFiles;
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
                    std::ostringstream fname;
                    fname << "Output/PYTHON_" << chosenParam << "_FUNC.txt";
                    std::ofstream out(fname.str());
                    std::unordered_set<std::string> funcFiles;
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
                std::cout << "Fertig fuer app." << chosenParam << "\n";
                setColor(7);
            }
        }
        else {
            std::cerr << "Unbekannte Auswahl!\n";
        }
    }

    return 0;
}

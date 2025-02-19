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

void printProgress(size_t current, size_t total, int width = 50) {
    static auto lastUpdate = high_resolution_clock::now();
    auto now = high_resolution_clock::now();
    if (duration_cast<milliseconds>(now - lastUpdate).count() < 100)
        return;
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

// -----------------------------------------------------
// Data structures
// -----------------------------------------------------
struct CodeBlock {
    string filename;
    string content;
};

struct ParseResult {
    vector<CodeBlock> defineBlocks;
    vector<CodeBlock> functionBlocks;
};

// -----------------------------------------------------
// TEIL A: C++-Parsing (#if define, function-blocks)
// -----------------------------------------------------

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
                        potentialFunctionHead = true;
                        potentialHeadBuffer.str("");
                        potentialHeadBuffer.clear();
                        potentialHeadBuffer << line;
                    }
                }
            }
        }
        else {
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

void parseWorker(const vector<string>& files,
    size_t from,
    size_t to,
    const regex& startDefineRegex,
    atomic<size_t>& processed,
    size_t totalLines,
    ParseResult& result)
{
    for (size_t i = from; i < to; ++i) {
        const auto& filename = files[i];
        size_t lineCount = 0;
        auto pr = parseFileSinglePass(filename, startDefineRegex, processed, totalLines, lineCount);
        result.defineBlocks.insert(result.defineBlocks.end(),
            pr.first.begin(), pr.first.end());
        result.functionBlocks.insert(result.functionBlocks.end(),
            pr.second.begin(), pr.second.end());
        printProgress(processed.load(), totalLines);
    }
}

size_t countTotalLines(const vector<string>& files) {
    size_t total = 0;
    for (auto& f : files) {
        ifstream ifs(f);
        if (!ifs.is_open()) continue;
        string tmp;
        while (getline(ifs, tmp)) {
            total++;
        }
    }
    return total;
}

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

pair<vector<CodeBlock>, vector<CodeBlock>>
parseAllFilesMultiThread(const vector<string>& files, const string& define)
{
    regex startDefineRegex = createConditionalRegex(define);
    cout << "Zaehle Zeilen...\n";
    size_t totalLines = countTotalLines(files);
    cout << "Gesamt: " << totalLines << " Zeilen.\n";

    unsigned int hwThreads = thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 2;
    size_t numThreads = std::min<size_t>(hwThreads, files.size());
    cout << "Starte " << numThreads << " Thread(s)...\n";

    vector<ParseResult> partialResults(numThreads);
    vector<thread> threads;
    atomic<size_t> processed{ 0 };
    size_t chunkSize = (files.size() + numThreads - 1) / numThreads;

    auto startTime = high_resolution_clock::now();
    for (size_t t = 0; t < numThreads; ++t) {
        size_t from = t * chunkSize;
        if (from >= files.size()) break;
        size_t to = std::min<size_t>(from + chunkSize, files.size());
        threads.emplace_back(parseWorker,
            cref(files),
            from,
            to,
            cref(startDefineRegex),
            ref(processed),
            totalLines,
            ref(partialResults[t]));
    }
    for (auto& th : threads) {
        th.join();
    }

    vector<CodeBlock> allDefineBlocks;
    vector<CodeBlock> allFunctionBlocks;
    for (auto& pr : partialResults) {
        allDefineBlocks.insert(allDefineBlocks.end(),
            pr.defineBlocks.begin(), pr.defineBlocks.end());
        allFunctionBlocks.insert(allFunctionBlocks.end(),
            pr.functionBlocks.begin(), pr.functionBlocks.end());
    }
    printProgress(totalLines, totalLines);
    cout << "\n";

    auto endTime = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(endTime - startTime).count();
    cout << "Parsing '" << define << "' fertig in " << ms << " ms\n";

    return make_pair(allDefineBlocks, allFunctionBlocks);
}

// -----------------------------------------------------
// TEIL B: PYTHON-Parsen
// -----------------------------------------------------
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

// parseSingleFile (python)
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

    // load all lines
    vector<string> lines;
    {
        string line;
        while (getline(ifs, line)) {
            lines.push_back(line);
        }
    }

    bool insideFunc = false;
    int funcIndent = 0;
    ostringstream currentFunc;
    bool functionRelevant = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        size_t oldVal = processed.fetch_add(1, memory_order_relaxed);
        if ((oldVal + 1) % 200 == 0) {
            printProgress(oldVal + 1, totalLines);
        }
        outLineCount++;

        const string& line = lines[i];

        // check def start
        if (regex_search(line, defRegex)) {
            // schließe ggf. vorherige Funktion ab
            if (insideFunc && functionRelevant) {
                CodeBlock cb;
                cb.filename = filename;
                cb.content = "##########\n" + filename + "\n##########\n" +
                    currentFunc.str();
                funcBlocks.push_back(cb);
            }
            // neue Funktion
            insideFunc = true;
            funcIndent = getIndent(line);
            currentFunc.str("");
            currentFunc.clear();
            currentFunc << line << "\n";
            functionRelevant = false;
            continue;
        }

        // prüfe, ob wir die aktuelle Funktion verlassen
        if (insideFunc) {
            int currentIndent = getIndent(line);
            // wenn wir eine Zeile mit Einrückung <= funcIndent sehen (und kein neuer def),
            // dann ist die Funktion zuende
            if (currentIndent <= funcIndent && !line.empty() && !regex_search(line, defRegex)) {
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
                // wir sind weiter in der Funktion
                currentFunc << line << "\n";
            }
        }

        // check if param in an if/elif
        smatch m;
        if (regex_search(line, m, ifParamRegex)) {
            // If-Block
            int ifIndent = getIndent(line);
            ostringstream blockContent;
            blockContent << line << "\n";

            size_t j = i + 1;
            for (; j < lines.size(); ++j) {
                int indentJ = getIndent(lines[j]);
                if (lines[j].empty()) {
                    if (indentJ < ifIndent) {
                        break;
                    }
                    blockContent << lines[j] << "\n";
                }
                else {
                    if (indentJ <= ifIndent) {
                        break;
                    }
                    blockContent << lines[j] << "\n";
                }
            }
            CodeBlock cb;
            cb.filename = filename;
            cb.content = "##########\n" + filename + "\n##########\n" +
                blockContent.str();
            ifBlocks.push_back(cb);

            // falls wir in einer Funktion sind -> relevant
            if (insideFunc) {
                functionRelevant = true;
            }
            // i hochsetzen
            i = j - 1;
        }
    }

    // Falls Dateiende, aber insideFunc = true
    if (insideFunc && functionRelevant) {
        CodeBlock cb;
        cb.filename = filename;
        cb.content = "##########\n" + filename + "\n##########\n" +
            currentFunc.str();
        funcBlocks.push_back(cb);
    }

    return make_pair(ifBlocks, funcBlocks);
}

// worker for python
void parsePythonWorker(const vector<string>& files,
    size_t from,
    size_t to,
    const string& param,
    atomic<size_t>& processed,
    size_t totalLines,
    ParseResult& result)
{
    for (size_t i = from; i < to; ++i) {
        const auto& filename = files[i];
        size_t lineCount = 0;
        auto pr = parsePythonFileSinglePass(filename, param, processed, totalLines, lineCount);
        result.defineBlocks.insert(result.defineBlocks.end(),
            pr.first.begin(), pr.first.end());
        result.functionBlocks.insert(result.functionBlocks.end(),
            pr.second.begin(), pr.second.end());
        printProgress(processed.load(), totalLines);
    }
}

pair<vector<CodeBlock>, vector<CodeBlock>>
parsePythonAllFilesMultiThread(const vector<string>& pyFiles, const string& param)
{
    // Count lines quickly
    size_t totalLines = 0;
    for (auto& f : pyFiles) {
        ifstream ifs(f);
        if (!ifs.is_open()) continue;
        string tmp;
        while (getline(ifs, tmp)) {
            totalLines++;
        }
    }

    cout << "Gesamt: " << totalLines << " Python-Zeilen.\n";

    unsigned int hwThreads = thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 2;
    size_t numThreads = std::min<size_t>(hwThreads, pyFiles.size());
    cout << "Starte " << numThreads << " Thread(s) (Python)...\n";

    vector<ParseResult> partialResults(numThreads);
    vector<thread> threads;
    atomic<size_t> processed{ 0 };
    size_t chunkSize = (pyFiles.size() + numThreads - 1) / numThreads;

    auto startTime = high_resolution_clock::now();
    for (size_t t = 0; t < numThreads; ++t) {
        size_t from = t * chunkSize;
        if (from >= pyFiles.size()) break;
        size_t to = std::min<size_t>(from + chunkSize, pyFiles.size());
        threads.emplace_back(parsePythonWorker,
            cref(pyFiles),
            from,
            to,
            cref(param),
            ref(processed),
            totalLines,
            ref(partialResults[t]));
    }
    for (auto& th : threads) {
        th.join();
    }

    vector<CodeBlock> allIfBlocks;
    vector<CodeBlock> allFuncBlocks;
    for (auto& pr : partialResults) {
        allIfBlocks.insert(allIfBlocks.end(),
            pr.defineBlocks.begin(), pr.defineBlocks.end());
        allFuncBlocks.insert(allFuncBlocks.end(),
            pr.functionBlocks.begin(), pr.functionBlocks.end());
    }
    printProgress(totalLines, totalLines);
    cout << "\n";

    auto endTime = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(endTime - startTime).count();
    cout << "Parsing (app." << param << ") fertig in " << ms << " ms\n";

    return make_pair(allIfBlocks, allFuncBlocks);
}

// sammle alle param aus if/elif app.xyz
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

// -----------------------------------------------------
// MAIN + Menu
// -----------------------------------------------------
int main() {
    fs::path startPath = fs::current_path();

    bool hasClientHeader = false;
    bool hasServerHeader = false;

    std::string clientHeaderName;
    std::string serverHeaderName;

    // Hier speichern wir nun alle gefundenen "root"-Ordner,
    // damit wir dem Benutzer ein Auswahl-Menü anbieten können,
    // wenn mehr als einer existiert.
    vector<string> possiblePythonRoots;

    // Rekursive Suche
    for (auto& p : fs::recursive_directory_iterator(startPath)) {
        if (fs::is_regular_file(p)) {
            std::string fn = p.path().filename().string();
            std::string lower = fn;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

            if (lower == "locale_inc.h") {
                hasClientHeader = true;
                // Wichtigen, vollständigen Pfad speichern
                clientHeaderName = p.path().string();
            }
            if (lower == "service.h" || lower == "commondefines.h") {
                hasServerHeader = true;
                // Ebenfalls vollständigen Pfad speichern
                serverHeaderName = p.path().string();
            }
        }

        // Prüfe, ob wir einen "root"-Ordner haben
        if (fs::is_directory(p) && p.path().filename() == "root") {
            possiblePythonRoots.push_back(p.path().string());
        }
    }

    // Nun entscheiden wir, ob und welches root-Verzeichnis genutzt wird
    bool hasPythonRoot = !possiblePythonRoots.empty();
    std::string pythonRoot;  // bleibt leer bis Auswahl getroffen ist

    if (hasPythonRoot) {
        // Falls nur ein root gefunden wurde, direkt übernehmen
        if (possiblePythonRoots.size() == 1) {
            pythonRoot = possiblePythonRoots[0];
        }
        // Falls mehrere vorhanden, Menüauswahl
        else {
            cout << "Es wurden mehrere 'root'-Verzeichnisse gefunden:\n";
            for (size_t i = 0; i < possiblePythonRoots.size(); ++i) {
                cout << (i + 1) << ". " << possiblePythonRoots[i] << "\n";
            }
            cout << "\nBitte wählen Sie eines aus (1-" << possiblePythonRoots.size() << "): ";
            int selection = 0;
            cin >> selection;

            // Grundlegende Validierung der Eingabe
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

        // ----------------------------------------
        // A) Client
        // ----------------------------------------
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

            // jetzt Menü für die defines
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
        // ----------------------------------------
        // B) Server
        // ----------------------------------------
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
        // ----------------------------------------
        // C) Python
        // ----------------------------------------
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

            // Untermenü: Parameter wählen
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

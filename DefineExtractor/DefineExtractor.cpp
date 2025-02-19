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

// -----------------------------------------------------
// Utilities for console colors & progress
// -----------------------------------------------------
static std::mutex consoleMutex;

#ifdef _WIN32
void setColor(int color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}
#else
void setColor(int color) {
    // Basic color mapping
    // 7 = default, 10 = bright green, 12 = bright red
    // We'll map these loosely to ANSI codes
    cout << "\033[" << color << "m";
}
#endif

void printProgress(size_t current, size_t total, int width = 50) {
    // limit refresh to every ~100ms
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
/**
 * Holds the filename + the captured text block (either a #if define block or a function block).
 */
struct CodeBlock {
    string filename;
    string content;
};

struct ParseResult {
    vector<CodeBlock> defineBlocks;
    vector<CodeBlock> functionBlocks;
};

// -----------------------------------------------------
// Regex Helpers for #if-block detection
// -----------------------------------------------------
static const regex endifRegex(R"(^\s*#\s*endif\b)",
    regex_constants::ECMAScript | regex_constants::optimize);

// We’ll use a simpler approach to detect *any* #if/#ifdef/#ifndef:
static const regex anyIfStartRegex(R"(^\s*#\s*(if|ifdef|ifndef)\b)",
    regex_constants::ECMAScript | regex_constants::optimize);

// #elif or #else do not start new nesting, so we ignore them for "++", but we do store them in the output as part of the block.

//
// For starting the define-block, we specifically look if the line
// references the chosen define in #if / #ifdef / #ifndef / #elif variants.
//
// Example pattern for "FOO":
//   #ifdef FOO
//   #ifndef FOO
//   #if defined(FOO)
//   #if FOO
//   #elif defined(FOO)
//   #elif FOO
//
// This is used only to detect the *start* of "insideDefineBlock".
//
regex createConditionalRegex(const string& define) {
    // We'll combine alternatives into one large pattern:
    //   #ifdef DEFINE
    //   #ifndef DEFINE
    //   #if defined(DEFINE)
    //   #elif defined(DEFINE)
    //   #if DEFINE
    //   #elif DEFINE
    //   ... with optional parentheses etc.
    //
    // Using a capturing group with \b or spaces. 
    //
    // Explanation of pattern sections:
    //  1) ^\s*#(ifdef|ifndef)\s+DEFINE\b
    //  2) ^\s*#(if|elif)\s+defined\s*\(\s*DEFINE\s*\)
    //  3) ^\s*#(if|elif)\s+defined\s+DEFINE
    //  4) ^\s*#(if|elif)\s+\(?\s*DEFINE\s*\)?
    //
    // Note: We combine them with '|', so if *any* part matches, it’s a "start".
    //
    ostringstream pattern;
    pattern
        << R"((^\s*#(ifdef|ifndef)\s+)" << define << R"(\b))"
        << R"(|)" // OR
        << R"((^\s*#(if|elif)\s+defined\s*\(\s*)" << define << R"(\s*\)))"
        << R"(|)"
        << R"((^\s*#(if|elif)\s+defined\s+)" << define << R"())"
        << R"(|)"
        << R"((^\s*#(if|elif)\s+\(?\s*)" << define << R"(\s*\)?))";
    return regex(pattern.str(), regex_constants::ECMAScript | regex_constants::optimize);
}

// -----------------------------------------------------
// Regex to detect function signatures (heuristic)
// -----------------------------------------------------
// Typical matches might be:
//   static inline int foo(...) {
//   virtual void bar(...) const {
//   MyClass::MyClass(...) : initializer {
//   int foo(...) {
//   friend Foo operator+(...) { 
//
// We'll try to handle multi-line by seeing if the line ends with '{', ';', or is incomplete.
static const regex functionHeadRegex(
    R"(^\s*(?:inline\s+|static\s+|virtual\s+|constexpr\s+|friend\s+|typename\s+|[\w:\*&<>]+\s+)*[\w:\*&<>]+\s+\w[\w:\*&<>]*\s*\([^)]*\)\s*(\{|;|$))",
    regex_constants::ECMAScript | regex_constants::optimize
);

// -----------------------------------------------------
// parseFileSinglePass
//   - Finds #if(define) blocks (including nested #ifs).
//   - Finds function blocks if they contain that #if(define).
// -----------------------------------------------------
pair<vector<CodeBlock>, vector<CodeBlock>>
parseFileSinglePass(const string& filename,
    const regex& startDefineRegex, // for checking #if referencing the chosen define
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

    // For function parsing:
    bool inFunction = false;
    int braceCount = 0;
    bool functionRelevant = false; // if we see #if(define) inside this function
    ostringstream currentFunc;

    // Multi-line function-head detection:
    bool potentialFunctionHead = false;
    ostringstream potentialHeadBuffer;

    string line;
    while (true) {
        if (!getline(ifs, line)) {
            break;
        }
        outLineCount++;

        // For progress
        size_t oldVal = processed.fetch_add(1, memory_order_relaxed);
        if ((oldVal + 1) % 200 == 0) {
            printProgress(oldVal + 1, totalLines);
        }

        // -----------------------------------------------------
        // 1) #if(define)-BLOCK logic
        // -----------------------------------------------------

        // If we're *not* yet inside a define-block, check if this line starts it:
        if (!insideDefineBlock) {
            if (regex_search(line, startDefineRegex)) {
                // We found #if or #ifdef referencing the user-chosen define.
                insideDefineBlock = true;
                defineNesting = 1;
                currentDefineBlock.str("");
                currentDefineBlock.clear();
                currentDefineBlock << line << "\n";
            }
        }
        else {
            // We are inside a define-block. Always store the line:
            currentDefineBlock << line << "\n";

            // Check if this line starts *any* #if/#ifdef/#ifndef => nesting++
            if (regex_search(line, anyIfStartRegex)) {
                defineNesting++;
            }
            // Check if #endif => nesting--
            else if (regex_search(line, endifRegex)) {
                defineNesting--;
                if (defineNesting <= 0) {
                    // We close out this define-block
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

        // -----------------------------------------------------
        // 2) Function-block logic
        //    We gather function bodies if they contain #if(define).
        // -----------------------------------------------------

        // We'll do a rough multi-line signature detection:

        // Helper booleans:
        bool lineHasBraceOrParen = (line.find('{') != string::npos ||
            line.find('}') != string::npos ||
            line.find('(') != string::npos);

        // Also see if line references the user-chosen define (to mark function relevant):
        bool lineMatchesDefine = regex_search(line, startDefineRegex);

        if (!inFunction) {
            // Not currently in a function body
            if (potentialFunctionHead) {
                // We have some partial function-head in 'potentialHeadBuffer'.
                // We see if this line includes '{' or ';' => that might finalize the signature.
                // (Heuristic: if we see '{', it means the function body starts.)
                // (If we see ';', it might be just a prototype => no function body.)
                // Or if the line still doesn't contain either, keep buffering.

                // We'll combine with previous lines:
                potentialHeadBuffer << "\n" << line;

                // Check if it ends with '{' or contains '{'
                bool hasOpenBrace = (line.find('{') != string::npos);
                bool hasSemicolon = (line.find(';') != string::npos);

                if (hasOpenBrace) {
                    // Start function body
                    inFunction = true;
                    braceCount = 0;
                    functionRelevant = false; // reset for new function
                    currentFunc.str("");
                    currentFunc.clear();

                    // Merge the potential head + this line
                    currentFunc << potentialHeadBuffer.str() << "\n";

                    // Count braces in this line:
                    for (char c : line) {
                        if (c == '{') braceCount++;
                        if (c == '}') braceCount--;
                    }
                    // Mark relevant if #if(define) found here:
                    if (lineMatchesDefine) {
                        functionRelevant = true;
                    }

                    // We consumed the potential head
                    potentialFunctionHead = false;
                    potentialHeadBuffer.str("");
                    potentialHeadBuffer.clear();
                }
                else if (hasSemicolon) {
                    // It's likely just a forward declaration or end of multiline prototype with no body
                    // We'll discard it
                    potentialFunctionHead = false;
                    potentialHeadBuffer.str("");
                    potentialHeadBuffer.clear();
                }
                else {
                    // We still do not have '{' or ';' => keep buffering
                    // We'll wait for next line
                }
            }
            else {
                // Check if the current line itself looks like a function head
                smatch match;
                if (regex_search(line, match, functionHeadRegex)) {
                    // match[1] is the group that might be '{' or ';' or empty
                    string trailingSymbol = match[1].str(); // might be '{', ';', or empty

                    if (trailingSymbol == "{") {
                        // The function body starts right away
                        inFunction = true;
                        braceCount = 0;
                        functionRelevant = false;
                        currentFunc.str("");
                        currentFunc.clear();

                        // Store the current line
                        currentFunc << line << "\n";

                        // Count braces
                        for (char c : line) {
                            if (c == '{') braceCount++;
                            if (c == '}') braceCount--;
                        }
                        if (lineMatchesDefine) {
                            functionRelevant = true;
                        }
                    }
                    else if (trailingSymbol == ";") {
                        // Just a forward declaration, do nothing
                    }
                    else {
                        // Possibly multi-line function signature; store in buffer
                        potentialFunctionHead = true;
                        potentialHeadBuffer.str("");
                        potentialHeadBuffer.clear();
                        potentialHeadBuffer << line;
                    }
                }
                else {
                    // Not matching functionHeadRegex => do nothing
                }
            }
        }
        else {
            // We are inside a function body
            currentFunc << line << "\n";
            if (lineMatchesDefine) {
                functionRelevant = true;
            }

            // Count braces in the line
            if (lineHasBraceOrParen) {
                for (char c : line) {
                    if (c == '{') braceCount++;
                    if (c == '}') braceCount--;
                }
            }

            // If braceCount is 0 => function ended
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

    } // end while(getline())

    return make_pair(defineBlocks, functionBlocks);
}

// -----------------------------------------------------
// Worker thread function
// -----------------------------------------------------
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

        // Merge results
        result.defineBlocks.insert(result.defineBlocks.end(),
            pr.first.begin(), pr.first.end());
        result.functionBlocks.insert(result.functionBlocks.end(),
            pr.second.begin(), pr.second.end());

        // Just to ensure progress bar updates
        printProgress(processed.load(), totalLines);
    }
}

// -----------------------------------------------------
// Count lines across all files for progress
// -----------------------------------------------------
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

// -----------------------------------------------------
// Read #define names from a given header
//   e.g. lines of form:  #define FOO
// -----------------------------------------------------
vector<string> readDefines(const string& filename) {
    vector<string> result;
    ifstream ifs(filename);
    if (!ifs.is_open()) return result;

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

// -----------------------------------------------------
// Ask user: Client or Server
//   Return the path to the discovered header (if found).
// -----------------------------------------------------
string findHeaderFile(bool& isClient) {
    // Example sets you can modify based on your environment
    unordered_set<string> clientFiles = { "locale_inc.h" };
    unordered_set<string> serverFiles = { "service.h", "commondefines.h" };

    vector<string> foundClientFiles;
    vector<string> foundServerFiles;

    for (const auto& entry : fs::recursive_directory_iterator(".")) {
        if (!fs::is_regular_file(entry)) continue;

        string filename = entry.path().filename().string();
        // Lowercase for comparison
        string lower = filename;
        transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (clientFiles.count(lower)) {
            foundClientFiles.push_back(entry.path().string());
        }
        if (serverFiles.count(lower)) {
            foundServerFiles.push_back(entry.path().string());
        }
    }

    // Show client option in green if found, else red
    setColor(foundClientFiles.empty() ? 12 : 10);
    cout << "1. Client\n";
    // Show server option
    setColor(foundServerFiles.empty() ? 12 : 10);
    cout << "2. Server\n";
    setColor(7);

    int choice;
    cout << "Choice: ";
    cin >> choice;

    if (choice == 1 && !foundClientFiles.empty()) {
        isClient = true;
        return foundClientFiles[0];
    }
    else if (choice == 2 && !foundServerFiles.empty()) {
        isClient = false;
        return foundServerFiles[0];
    }

    cerr << "Header file not found.\n";
    return "";
}

// -----------------------------------------------------
// Search .cpp / .h files recursively
// -----------------------------------------------------
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

// -----------------------------------------------------
// Multi-thread parse for a single define
// -----------------------------------------------------
pair<vector<CodeBlock>, vector<CodeBlock>>
parseAllFilesMultiThread(const vector<string>& files, const string& define)
{
    // 1) Create the regex for lines that start #if define
    regex startDefineRegex = createConditionalRegex(define);

    // 2) Count total lines (for progress bar)
    cout << "Zaehle Zeilen...\n";
    size_t totalLines = countTotalLines(files);
    cout << "Gesamt: " << totalLines << " Zeilen.\n";

    // 3) Create threads
    unsigned int hwThreads = thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 2; // fallback
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

    // Collect results
    vector<CodeBlock> allDefineBlocks;
    vector<CodeBlock> allFunctionBlocks;
    for (auto& pr : partialResults) {
        allDefineBlocks.insert(allDefineBlocks.end(),
            pr.defineBlocks.begin(), pr.defineBlocks.end());
        allFunctionBlocks.insert(allFunctionBlocks.end(),
            pr.functionBlocks.begin(), pr.functionBlocks.end());
    }

    // Final progress = 100%
    printProgress(totalLines, totalLines);
    cout << "\n";

    auto endTime = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(endTime - startTime).count();

    cout << "Parsing '" << define << "' fertig in " << ms << " ms\n";
    return make_pair(allDefineBlocks, allFunctionBlocks);
}

// -----------------------------------------------------
// Main menu loop
// -----------------------------------------------------
void showMenu(const vector<string>& defines,
    const vector<string>& files,
    bool isClient)
{
    if (files.empty() || defines.empty()) {
        cerr << "Keine Dateien oder keine Defines.\n";
        return;
    }

    fs::create_directory("Output");
    string prefix = isClient ? "CLIENT_" : "SERVER_";

    while (true) {
        cout << "\n#############################\n"
            << "#        Define Parser      #\n"
            << "#############################\n"
            << "Available #defines:\n";

        for (size_t i = 0; i < defines.size(); ++i) {
            cout << (i + 1) << ". " << defines[i] << "\n";
        }
        cout << "0. Exit\nYour choice: ";

        int choice;
        cin >> choice;
        if (!cin || choice == 0) {
            break;
        }
        if (choice < 0 || choice >(int)defines.size()) {
            cerr << "Ungueltige Auswahl\n";
            continue;
        }

        string define = defines[choice - 1];

        // Parse with multiple threads
        auto results = parseAllFilesMultiThread(files, define);
        auto& allDefineBlocks = results.first;
        auto& allFunctionBlocks = results.second;

        // Write to output files
        {
            ofstream outDef("Output/" + prefix + define + "_DEFINE.txt");
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
            ofstream outFunc("Output/" + prefix + define + "_FUNC.txt");
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

        // Short console summary
        setColor(10);
        unordered_set<string> combined;
        for (auto& b : allDefineBlocks)   combined.insert(b.filename);
        for (auto& b : allFunctionBlocks) combined.insert(b.filename);

        cout << "Gefunden: "
            << allDefineBlocks.size() << " Define-Block/Bloecke und "
            << allFunctionBlocks.size() << " Funktions-Bloecke.\n";
        cout << "  In insgesamt " << combined.size() << " Datei(en)\n";
        setColor(7);
    }
}

// -----------------------------------------------------
// main()
// -----------------------------------------------------
int main() {
    bool isClient = false;
    string headerFile = findHeaderFile(isClient);
    if (headerFile.empty()) {
        return 1;
    }

    auto defines = readDefines(headerFile);
    if (defines.empty()) {
        cerr << "Keine #define-Eintraege im Header gefunden!\n";
        return 2;
    }

    auto sourceFiles = findSourceFiles();
    if (sourceFiles.empty()) {
        cerr << "Keine .cpp/.h-Dateien gefunden.\n";
        return 3;
    }

    showMenu(defines, sourceFiles, isClient);

    return 0;
}

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
#include <numeric>

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

#define BUFFER_SIZE 8192

/*******************************************************
 * PLATFORM-SPECIFIC: clearConsole()
 *  - Clears the screen on Windows vs. Linux/Unix
 *******************************************************/
#ifdef _WIN32
static inline void clearConsole() {
    system("cls");
}
#else
static inline void clearConsole() {
    system("clear");
}
#endif

/*******************************************************
 * Simple color functions for console output
 *  - 7 = default, 10 = green, 12 = red
 *******************************************************/
static std::mutex consoleMutex;
#ifdef _WIN32
static inline void setColor(int color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}
#else
static inline void setColor(int color) {
    // Minimal ANSI codes
    std::cout << "\033[" << color << "m";
}
#endif

/*******************************************************
 * printProgress():
 *   Thread-safe progress bar. Avoids too-frequent updates.
 *******************************************************/
void printProgress(size_t current, size_t total, int width = 50) {
    static auto lastUpdate = high_resolution_clock::now();
    auto now = high_resolution_clock::now();
    // update only every ~100ms
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
    std::cout << "] " << int(ratio * 100.0f) << " %\r" << std::flush;
}

/*******************************************************
 * readBufferedFile(filename, lines):
 *
 * Reads a file in chunks of BUFFER_SIZE bytes to minimize
 * file I/O overhead. Stores all lines in a `std::vector<std::string>`.
 *******************************************************/
void readBufferedFile(const std::string& filename, std::vector<std::string>& lines) {
    std::ifstream file(filename, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Unable to open file: " << filename << "\n";
        return;
    }

    std::vector<char> buffer(BUFFER_SIZE);
    std::string line;
    std::string leftover;

    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        size_t bytesRead = file.gcount();
        std::string chunk(buffer.data(), bytesRead);

        // Append leftover from previous chunk if needed
        chunk = leftover + chunk;
        leftover.clear();
        size_t pos = 0;

        while ((pos = chunk.find('\n')) != std::string::npos) {
            line = chunk.substr(0, pos);
            chunk.erase(0, pos + 1);
            lines.push_back(line); // Store the extracted line
        }

        // Store incomplete line (if any) for next chunk processing
        leftover = chunk;
    }

    if (!leftover.empty()) {
        lines.push_back(leftover); // Add the last remaining line if not terminated with `\n`
    }
}

/*******************************************************
 * Data Structures
 *******************************************************/
struct CodeBlock {
    std::string filename;
    std::string content;
};

/*******************************************************
 * Global line-count cache to avoid re-reading files
 *******************************************************/
static std::mutex lineCountCacheMutex;
static std::unordered_map<std::string, size_t> lineCountCache;

/*******************************************************
 * getFileLineCount(filename):
 *   Returns line count for a single file. Uses a global
 *   cache to avoid re-counting the same file multiple times.
 *******************************************************/
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
        std::lock_guard<std::mutex> lk(lineCountCacheMutex);
        lineCountCache[filename] = 0;
        return 0;
    }

    size_t count = 0;
    std::string tmp;
    while (std::getline(ifs, tmp)) {
        count++;
    }

    {
        std::lock_guard<std::mutex> lk(lineCountCacheMutex);
        lineCountCache[filename] = count;
    }
    return count;
}

/*******************************************************
 * getTotalLineCount(files):
 *   Returns sum of line counts for all given files
 *******************************************************/
size_t getTotalLineCount(const std::vector<std::string>& files)
{
    size_t total = 0;
    for (auto& f : files) {
        total += getFileLineCount(f);
    }
    return total;
}

/*******************************************************
 * REGEX-based detection for #if <DEFINE> and function heads (C++)
 *******************************************************/
static const std::regex endifRegex(R"(^\s*#\s*endif\b)",
    std::regex_constants::ECMAScript | std::regex_constants::optimize);
static const std::regex anyIfStartRegex(R"(^\s*#\s*(if|ifdef|ifndef)\b)",
    std::regex_constants::ECMAScript | std::regex_constants::optimize);

/** createConditionalRegex(define):
 *   Build a combined regex that matches #ifdef <define>,
 *   #if <define>, etc.
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
    return std::regex(pattern.str(),
        std::regex_constants::ECMAScript | std::regex_constants::optimize);
}

/** Regex that detects a typical C++ function header. (heuristic) */
static const std::regex functionHeadRegex(
    R"(^\s*(?:inline\s+|static\s+|virtual\s+|constexpr\s+|friend\s+|typename\s+|[\w:\*&<>]+\s+)*[\w:\*&<>]+\s+\w[\w:\*&<>]*\s*\([^)]*\)\s*(\{|;|$))",
    std::regex_constants::ECMAScript | std::regex_constants::optimize
);

/*******************************************************
 * parseFileSinglePass():
 *   Parse a C++ file for:
 *     1) #if <DEFINE> blocks
 *     2) Functions containing the define
 *******************************************************/
std::pair<std::vector<CodeBlock>, std::vector<CodeBlock>>
parseFileSinglePass(const std::string& filename,
    const std::regex& startDefineRegex,
    std::atomic<size_t>& processed,
    size_t totalLines,
    size_t& outLineCount)
{
    std::vector<CodeBlock> defineBlocks;
    std::vector<CodeBlock> functionBlocks;

    std::vector<std::string> lines;
    readBufferedFile(filename, lines);

    bool insideDefineBlock = false;
    int  defineNesting = 0;
    std::ostringstream currentDefineBlock;

    bool inFunction = false;
    int braceCount = 0;
    bool functionRelevant = false;
    std::ostringstream currentFunc;

    bool potentialFunctionHead = false;
    std::ostringstream potentialHeadBuffer;

    for (const auto&line : lines) {
        outLineCount++;

        size_t oldVal = processed.fetch_add(1, std::memory_order_relaxed);
        if ((oldVal + 1) % 200 == 0) {
            printProgress(oldVal + 1, totalLines);
        }

        // Check for the specific #if <DEFINE>
        if (!insideDefineBlock) {
            if ((line.find("#if ") != std::string::npos ||
                line.find("#ifdef ") != std::string::npos ||
                line.find("#ifndef ") != std::string::npos) &&
                std::regex_search(line, startDefineRegex))
            {
                insideDefineBlock = true;
                defineNesting = 1;
                currentDefineBlock.str("");
                currentDefineBlock.clear();
                currentDefineBlock << line << "\n";
            }
        }
        else {
            // We are inside a #if <DEFINE> block
            currentDefineBlock << line << "\n";
            if (std::regex_search(line, anyIfStartRegex)) {
                defineNesting++;
            }
            else if (line.find("#endif") != std::string::npos) {
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

        // Functions
        bool lineHasBraceOrParen = (line.find('{') != std::string::npos ||
            line.find('}') != std::string::npos ||
            line.find('(') != std::string::npos);
        bool lineMatchesDefine = std::regex_search(line, startDefineRegex);

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
                    // forward-declaration, not a real function body
                    potentialFunctionHead = false;
                    potentialHeadBuffer.str("");
                    potentialHeadBuffer.clear();
                }
            }
            else {
                std::smatch match;
                if (std::regex_search(line, match, functionHeadRegex)) {
                    std::string trailingSymbol = match[1].str();
                    if (trailingSymbol == "{") {
                        // function starts
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
                        if (lineMatchesDefine) functionRelevant = true;
                    }
                    else if (trailingSymbol == ";") {
                        // just a declaration
                    }
                    else {
                        // possibly multi-line function head
                        potentialFunctionHead = true;
                        potentialHeadBuffer.str("");
                        potentialHeadBuffer.clear();
                        potentialHeadBuffer << line;
                    }
                }
            }
        }
        else {
            // we are inside a function
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
                // function ends
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
    return { defineBlocks, functionBlocks };
}

/*******************************************************
 * Python scanning: if app.xyz + function blocks
 *******************************************************/
static const std::regex pythonIfAppRegex(
    R"((?:if|elif)\s*\(?\s*app\.(\w+))",
    std::regex_constants::ECMAScript | std::regex_constants::optimize);
static const std::regex defRegex(R"(^\s*def\s+[\w_]+)");

/** Simple indentation check: each tab counts as 4 spaces. */
int getIndent(const std::string& ln) {
    return std::accumulate(ln.begin(), ln.end(), 0, [](int sum, char c) {
        return sum + (c == ' ' ? 1 : (c == '\t' ? 4 : 0));
        });
}

/** parsePythonFileSinglePass():
 *   Looks for lines containing "if app.<param>" and collects
 *   the subsequent indented block. Also detects functions
 *   containing such an if-block.
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
        return { ifBlocks, funcBlocks };
    }

    // build a quick regex for "if app.<param>"
    std::ostringstream oss;
    oss << R"((?:if|elif)\s*\(?\s*app\.)" << param << R"(\b)";
    std::regex ifParamRegex(oss.str(), std::regex_constants::ECMAScript | std::regex_constants::optimize);

    bool insideFunc = false;
    int  funcIndent = 0;
    bool functionRelevant = false;
    std::ostringstream currentFunc;

    std::string line;
    while (true) {
        std::streampos currentPos = ifs.tellg();
        if (!std::getline(ifs, line)) {
            break;
        }
        outLineCount++;

        size_t oldVal = processed.fetch_add(1, std::memory_order_relaxed);
        if ((oldVal + 1) % 200 == 0) {
            printProgress(oldVal + 1, totalLines);
        }

        // start of a def?
        if (std::regex_search(line, defRegex)) {
            // close out any previous function
            if (insideFunc && functionRelevant) {
                CodeBlock cb;
                cb.filename = filename;
                cb.content = "##########\n" + filename + "\n##########\n" +
                    currentFunc.str();
                funcBlocks.push_back(cb);
            }
            insideFunc = true;
            funcIndent = getIndent(line);
            functionRelevant = false;
            currentFunc.str("");
            currentFunc.clear();
            currentFunc << line << "\n";
            continue;
        }

        if (insideFunc) {
            int currentIndent = getIndent(line);
            bool nextDef = std::regex_search(line, defRegex);
            // if indentation <= function's indentation => function ends
            if (!line.empty() && currentIndent <= funcIndent && !nextDef) {
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
                currentFunc << line << "\n";
            }
        }

        // if app.<param> => collect block
        if (std::regex_search(line, ifParamRegex)) {
            int ifIndent = getIndent(line);
            std::ostringstream blockContent;
            blockContent << line << "\n";

            // subsequent lines with higher indent => part of block
            while (true) {
                std::streampos pos = ifs.tellg();
                std::string nextLine;
                if (!std::getline(ifs, nextLine)) {
                    break;
                }
                outLineCount++;

                size_t oldVal2 = processed.fetch_add(1, std::memory_order_relaxed);
                if ((oldVal2 + 1) % 200 == 0) {
                    printProgress(oldVal2 + 1, totalLines);
                }

                int indentJ = getIndent(nextLine);
                if (!nextLine.empty() && indentJ <= ifIndent) {
                    ifs.seekg(pos);
                    break;
                }
                blockContent << nextLine << "\n";
            }

            CodeBlock cb;
            cb.filename = filename;
            cb.content = "##########\n" + filename + "\n##########\n" +
                blockContent.str();
            ifBlocks.push_back(cb);

            if (insideFunc) {
                // this means function is relevant
                functionRelevant = true;
            }
        }
    }

    // close out a function at EOF if needed
    if (insideFunc && functionRelevant) {
        CodeBlock cb;
        cb.filename = filename;
        cb.content = "##########\n" + filename + "\n##########\n" +
            currentFunc.str();
        funcBlocks.push_back(cb);
    }

    return { ifBlocks, funcBlocks };
}

/*******************************************************
 * collectPythonParameters():
 *   Scans all .py files for lines: if app.<XYZ>
 *   Gathers unique "XYZ" parameters
 *******************************************************/
std::unordered_set<std::string> collectPythonParameters(const std::vector<std::string>& pyFiles) {
    std::unordered_set<std::string> params;

    static const std::unordered_set<std::string> blacklist = {
        "loggined", "VK_UP", "VK_RIGHT", "VK_LEFT", "VK_HOME", "VK_END",
        "VK_DOWN", "VK_DELETE", "TARGET", "SELL", "BUY", "DIK_DOWN",
        "DIK_F1", "DIK_F2", "DIK_F3", "DIK_F4", "DIK_H", "DIK_LALT",
        "DIK_LCONTROL", "DIK_RETURN", "DIK_SYSRQ", "DIK_UP", "DIK_V",
        "GetGlobalTime","GetTime","IsDevStage","IsEnableTestServerFlag",
        "IsExistFile","IsPressed","IsWebPageMode",
    };

    for (auto& f : pyFiles) {
        std::ifstream ifs(f);
        if (!ifs.is_open()) continue;

        std::string line;
        while (std::getline(ifs, line)) {
            std::smatch m;
            size_t pos = line.find("if app.");
            if (pos != std::string::npos) {
                size_t start = pos + 7;
                size_t end = line.find_first_of(" ():", start);
                std::string param = line.substr(start, end - start);

                if (!param.empty() && blacklist.find(param) == blacklist.end()) {
                    params.insert(param);
                }
            }
            else if (std::regex_search(line, pythonIfAppRegex)) {
                std::smatch m;
                if (std::regex_search(line, m, pythonIfAppRegex) && m.size() > 1) {
                    std::string param = m[1].str();
                    if (blacklist.find(param) == blacklist.end()) {
                        params.insert(param);
                    }
                }
            }
        }
    }
    return params;
}

/*******************************************************
 * Multi-threaded parsing (C++) to find #if <define> blocks + relevant functions
 *******************************************************/
static std::atomic<size_t> nextFileIndex{ 0 };

void parseWorkerDynamic(const std::vector<std::string>& files,
    const std::regex& startDefineRegex,
    std::atomic<size_t>& processed,
    size_t totalLines,
    std::vector<CodeBlock>& defineBlocksOut,
    std::vector<CodeBlock>& functionBlocksOut)
{
    std::vector<CodeBlock> localDefine;
    std::vector<CodeBlock> localFunc;

    while (true) {
        size_t idx = nextFileIndex.fetch_add(1, std::memory_order_relaxed);
        if (idx >= files.size()) {
            break;
        }
        const auto& filename = files[idx];

        size_t lineCountThisFile = 0;
        auto pr = parseFileSinglePass(filename, startDefineRegex,
            processed, totalLines, lineCountThisFile);

        localDefine.insert(localDefine.end(), pr.first.begin(), pr.first.end());
        localFunc.insert(localFunc.end(), pr.second.begin(), pr.second.end());
        printProgress(processed.load(), totalLines);
    }

    // lock to merge local results into global
    std::lock_guard<std::mutex> lock(consoleMutex);
    defineBlocksOut.insert(defineBlocksOut.end(), localDefine.begin(), localDefine.end());
    functionBlocksOut.insert(functionBlocksOut.end(), localFunc.begin(), localFunc.end());
}

/** parseAllFilesMultiThread(files, define):
 *   Spawns threads, reads all .h/.cpp in 'files' and returns
 *   matched #if <define> blocks + function blocks.
 */
std::pair<std::vector<CodeBlock>, std::vector<CodeBlock>>
parseAllFilesMultiThread(const std::vector<std::string>& files, const std::string& define)
{
    auto startDefineRegex = createConditionalRegex(define);

    std::cout << "Counting total lines...\n";
    size_t totalLines = getTotalLineCount(files);
    std::cout << "Total lines: " << totalLines << "\n";

    unsigned int hwThreads = std::thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 2;
    size_t numThreads = std::min<size_t>(hwThreads, files.size());
    std::cout << "Starting " << numThreads << " thread(s)...\n";

    std::atomic<size_t> processed{ 0 };
    std::vector<CodeBlock> allDefineBlocks;
    std::vector<CodeBlock> allFunctionBlocks;

    nextFileIndex.store(0);

    std::vector<std::thread> threads;
    auto startTime = high_resolution_clock::now();
    for (size_t t = 0; t < numThreads; ++t) {
        threads.emplace_back(parseWorkerDynamic,
            std::cref(files),
            std::cref(startDefineRegex),
            std::ref(processed),
            totalLines,
            std::ref(allDefineBlocks),
            std::ref(allFunctionBlocks));
    }
    for (auto& th : threads) {
        th.join();
    }

    printProgress(totalLines, totalLines);
    std::cout << "\n";

    auto endTime = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(endTime - startTime).count();
    std::cout << "Parsing define '" << define << "' finished in " << ms << " ms\n";

    return { allDefineBlocks, allFunctionBlocks };
}

/*******************************************************
 * Multi-threaded parsing (Python)
 *******************************************************/
static std::atomic<size_t> nextPyFileIndex{ 0 };

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
            break;
        }
        const auto& filename = files[idx];

        size_t lineCountThisFile = 0;
        auto pr = parsePythonFileSinglePass(filename, param,
            processed, totalLines,
            lineCountThisFile);

        localIf.insert(localIf.end(), pr.first.begin(), pr.first.end());
        localFunc.insert(localFunc.end(), pr.second.begin(), pr.second.end());
        printProgress(processed.load(), totalLines);
    }

    std::lock_guard<std::mutex> lock(consoleMutex);
    ifBlocksOut.insert(ifBlocksOut.end(), localIf.begin(), localIf.end());
    funcBlocksOut.insert(funcBlocksOut.end(), localFunc.begin(), localFunc.end());
}

/** parsePythonAllFilesMultiThread(pyFiles, param):
 *   Spawns threads to parse all .py files
 *   searching for if app.<param> + relevant functions
 */
std::pair<std::vector<CodeBlock>, std::vector<CodeBlock>>
parsePythonAllFilesMultiThread(const std::vector<std::string>& pyFiles, const std::string& param)
{
    std::cout << "Counting total lines (Python)...\n";
    size_t totalLines = getTotalLineCount(pyFiles);
    std::cout << "Total Python lines: " << totalLines << "\n";

    unsigned int hwThreads = std::thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 2;
    size_t numThreads = std::min<size_t>(hwThreads, pyFiles.size());
    std::cout << "Starting " << numThreads << " thread(s) for Python...\n";

    std::atomic<size_t> processed{ 0 };
    std::vector<CodeBlock> allIfBlocks;
    std::vector<CodeBlock> allFuncBlocks;

    nextPyFileIndex.store(0);

    std::vector<std::thread> threads;
    auto startTime = high_resolution_clock::now();
    for (size_t t = 0; t < numThreads; ++t) {
        threads.emplace_back(parsePythonWorkerDynamic,
            std::cref(pyFiles),
            std::cref(param),
            std::ref(processed),
            totalLines,
            std::ref(allIfBlocks),
            std::ref(allFuncBlocks));
    }
    for (auto& th : threads) {
        th.join();
    }

    printProgress(totalLines, totalLines);
    std::cout << "\n";

    auto endTime = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(endTime - startTime).count();
    std::cout << "Parsing (app." << param << ") finished in " << ms << " ms\n";

    return { allIfBlocks, allFuncBlocks };
}

/*******************************************************
 * findClientHeaderInUserInterface():
 *   Recursively scans the given path for "locale_inc.h"
 *   in any subdirectory that includes "UserInterface" in its path
 *******************************************************/
void findClientHeaderInUserInterface(const fs::path& startPath,
    bool& hasClientHeader,
    std::string& clientHeaderName)
{
    hasClientHeader = false;
    clientHeaderName.clear();

    try {
        for (auto& p : fs::recursive_directory_iterator(startPath,
            fs::directory_options::skip_permission_denied))
        {
            try {
                if (fs::is_symlink(p.path())) continue;
                auto st = p.status();
                if (!fs::is_regular_file(st)) continue;

                auto rel = p.path().string();
                std::string lowerRel;
                lowerRel.reserve(rel.size());
                for (char c : rel) {
                    lowerRel.push_back(std::tolower((unsigned char)c));
                }
                // must contain "userinterface"
                if (lowerRel.find("userinterface") == std::string::npos) {
                    continue;
                }
                // check file
                auto fn = p.path().filename().string();
                std::string lowerFn;
                for (char c : fn) {
                    lowerFn.push_back(std::tolower((unsigned char)c));
                }
                if (lowerFn == "locale_inc.h") {
                    hasClientHeader = true;
                    clientHeaderName = p.path().string();
                    return;
                }
            }
            catch (...) { continue; }
        }
    }
    catch (...) {}
}

/*******************************************************
 * findServerHeaderInCommon():
 *   Recursively scans for "service.h" or "commondefines.h"
 *   in a path containing "common" in its folder name
 *******************************************************/
void findServerHeaderInCommon(const fs::path& startPath,
    bool& hasServerHeader,
    std::string& serverHeaderName)
{
    hasServerHeader = false;
    serverHeaderName.clear();

    try {
        for (auto& p : fs::recursive_directory_iterator(startPath,
            fs::directory_options::skip_permission_denied))
        {
            try {
                if (fs::is_symlink(p.path())) continue;
                auto st = p.status();
                if (!fs::is_regular_file(st)) continue;

                auto rel = p.path().string();
                std::string lowerRel;
                lowerRel.reserve(rel.size());
                for (char c : rel) {
                    lowerRel.push_back(std::tolower((unsigned char)c));
                }
                // must contain "common"
                if (lowerRel.find("common") == std::string::npos) {
                    continue;
                }
                // check if service.h or commondefines.h
                auto fn = p.path().filename().string();
                std::string lowerFn;
                for (char c : fn) {
                    lowerFn.push_back(std::tolower((unsigned char)c));
                }
                if (lowerFn == "service.h" || lowerFn == "commondefines.h") {
                    hasServerHeader = true;
                    serverHeaderName = p.path().string();
                    return;
                }
            }
            catch (...) { continue; }
        }
    }
    catch (...) {}
}

/*******************************************************
 * findPythonRoots():
 *   Lists subfolders named exactly "root" (not recursing).
 *   You could adjust to do a deeper search if needed.
 *******************************************************/
void findPythonRoots(const fs::path& startPath,
    std::vector<std::string>& pythonRoots)
{
    pythonRoots.clear();
    try {
        for (auto& p : fs::directory_iterator(startPath,
            fs::directory_options::skip_permission_denied))
        {
            try {
                if (!fs::is_directory(p.path())) continue;
                auto wdir = p.path().filename().wstring();
                std::wstring lowerW;
                lowerW.reserve(wdir.size());
                for (wchar_t wc : wdir) {
                    lowerW.push_back(std::tolower(wc));
                }
                if (lowerW == L"root") {
                    pythonRoots.push_back(p.path().string());
                }
            }
            catch (...) { continue; }
        }
    }
    catch (...) {}
}

/*******************************************************
 * findSourceFiles(path):
 *   Recursively collects all .h / .cpp files from startRoot
 *******************************************************/
std::vector<std::string> findSourceFiles(const fs::path& startRoot)
{
    std::vector<std::string> result;
    static const std::unordered_set<std::string> validExtensions = { ".cpp", ".h" };

    try {
        for (auto& p : fs::recursive_directory_iterator(startRoot, fs::directory_options::skip_permission_denied))
        {
            try {
                if (fs::is_symlink(p.path())) continue;
                if (fs::is_regular_file(p.path())) {
                    std::string ext = p.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (validExtensions.count(ext)) {
                        result.push_back(p.path().string());
                    }
                }
            }
            catch (...) { continue; }
        }
    }
    catch (...) {}

    return result;
}

/*******************************************************
 * readDefines(filename):
 *   Collects #define <NAME> from a single header file
 *******************************************************/
std::vector<std::string> readDefines(const std::string& filename) {
    std::vector<std::string> result;
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
        std::cerr << "Could not open " << filename << "!\n";
        return result;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        size_t pos = line.find("#define ");
        if (pos != std::string::npos) {
            std::istringstream iss(line.substr(pos + 8)); // Nach "#define "
            std::string defineName;
            iss >> defineName;
            if (!defineName.empty()) {
                result.push_back(defineName);
            }
        }
    }
    return result;
}

/*******************************************************
 * getSubdirectoriesOfCurrentPath():
 *   Non-recursive listing of all subdirectories in the
 *   current working directory (where the .exe is placed).
 *******************************************************/
std::vector<fs::path> getSubdirectoriesOfCurrentPath()
{
    std::vector<fs::path> dirs;
    auto cur = fs::current_path();
    for (auto& p : fs::directory_iterator(cur, fs::directory_options::skip_permission_denied))
    {
        if (fs::is_directory(p.path())) {
            dirs.push_back(p.path());
        }
    }
    // sort them by name for consistent ordering
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

/*******************************************************
 * main():
 *   1) Show subdirectories in the current folder
 *   2) Let the user pick which folder is for Client, which for Server, etc.
 *   3) Mark them in red or green (set / not set).
 *   4) Then proceed to parse code, searching for defines or python params.
 *******************************************************/
int main()
{
    bool hasClientHeader = false;
    bool hasServerHeader = false;
    bool hasPythonRoot = false;

    std::string clientHeaderName;
    std::string serverHeaderName;
    std::string chosenPythonRoot;

    fs::path clientPath;
    fs::path serverPath;

    // get subdirectories of the current folder
    auto subdirs = getSubdirectoriesOfCurrentPath();

    while (true) {
        // clear console to keep the menu clean
        clearConsole();

        // Show current states in color
        std::cout << "===============================\n";
        std::cout << "     P A T H   S E T T I N G S \n";
        std::cout << "===============================\n\n";

        std::cout << "Client Path: ";
        if (!hasClientHeader) {
            setColor(12); // red
            std::cout << "(not set)\n";
        }
        else {
            setColor(10); // green
            std::cout << clientPath.string() << "\n";
        }
        setColor(7);

        std::cout << "Server Path: ";
        if (!hasServerHeader) {
            setColor(12);
            std::cout << "(not set)\n";
        }
        else {
            setColor(10);
            std::cout << serverPath.string() << "\n";
        }
        setColor(7);

        std::cout << "Python Root: ";
        if (!hasPythonRoot) {
            setColor(12);
            std::cout << "(not set)\n";
        }
        else {
            setColor(10);
            std::cout << chosenPythonRoot << "\n";
        }
        setColor(7);

        std::cout << "\n1) Select Client Path";
        // If set, show (set) in green
        if (hasClientHeader) {
            setColor(10); std::cout << " (set)";
        }
        setColor(7); std::cout << "\n";

        std::cout << "2) Select Server Path";
        if (hasServerHeader) {
            setColor(10); std::cout << " (set)";
        }
        setColor(7); std::cout << "\n";

        std::cout << "3) Select Python Root";
        if (hasPythonRoot) {
            setColor(10); std::cout << " (set)";
        }
        setColor(7); std::cout << "\n";

        std::cout << "4) -> Main Menu\n";
        std::cout << "0) Exit\n";
        std::cout << "Choice: ";

        int configChoice;
        std::cin >> configChoice;
        if (!std::cin) {
            std::cin.clear();
            std::cin.ignore(10000, '\n');
            continue;
        }
        std::cin.ignore(10000, '\n');

        if (configChoice == 0) {
            std::cout << "Exiting.\n";
            return 0;
        }
        else if (configChoice == 4) {
            // Go to the main menu
        }
        else if (configChoice == 1) {
            // select client path from subdirs
            clearConsole();
            if (subdirs.empty()) {
                std::cout << "No subdirectories found near the .exe!\n";
                std::cout << "Press ENTER to continue...\n";
                std::cin.ignore(10000, '\n');
                continue;
            }
            std::cout << "Available subdirectories:\n";
            for (size_t i = 0; i < subdirs.size(); ++i) {
                std::cout << (i + 1) << ") " << subdirs[i].filename().string() << "\n";
            }
            std::cout << "Select index (0=cancel): ";
            int sel;
            std::cin >> sel;
            if (!std::cin || sel <= 0 || sel > (int)subdirs.size()) {
                std::cin.clear();
                std::cin.ignore(10000, '\n');
                std::cout << "Cancelled.\n";
            }
            else {
                clientPath = subdirs[sel - 1];
                hasClientHeader = false;
                clientHeaderName.clear();
                findClientHeaderInUserInterface(clientPath, hasClientHeader, clientHeaderName);
                if (hasClientHeader) {
                    std::cout << "Found locale_inc.h at: " << clientHeaderName << "\n";
                }
                else {
                    std::cout << "locale_inc.h not found in that folder.\n";
                }
            }
            std::cout << "Press ENTER to continue...\n";
            std::cin.ignore(10000, '\n');
            continue;
        }
        else if (configChoice == 2) {
            // select server path
            clearConsole();
            if (subdirs.empty()) {
                std::cout << "No subdirectories found near the .exe!\n";
                std::cout << "Press ENTER to continue...\n";
                std::cin.ignore(10000, '\n');
                continue;
            }
            std::cout << "Available subdirectories:\n";
            for (size_t i = 0; i < subdirs.size(); ++i) {
                std::cout << (i + 1) << ") " << subdirs[i].filename().string() << "\n";
            }
            std::cout << "Select index (0=cancel): ";
            int sel;
            std::cin >> sel;
            if (!std::cin || sel <= 0 || sel > (int)subdirs.size()) {
                std::cin.clear();
                std::cin.ignore(10000, '\n');
                std::cout << "Cancelled.\n";
            }
            else {
                serverPath = subdirs[sel - 1];
                hasServerHeader = false;
                serverHeaderName.clear();
                findServerHeaderInCommon(serverPath, hasServerHeader, serverHeaderName);
                if (hasServerHeader) {
                    std::cout << "Found service.h/commondefines.h at: " << serverHeaderName << "\n";
                }
                else {
                    std::cout << "No service.h/commondefines.h found.\n";
                }
            }
            std::cout << "Press ENTER to continue...\n";
            std::cin.ignore(10000, '\n');
            continue;
        }
        else if (configChoice == 3) {
            // select python root
            clearConsole();
            if (subdirs.empty()) {
                std::cout << "No subdirectories found near the .exe!\n";
                std::cout << "Press ENTER to continue...\n";
                std::cin.ignore(10000, '\n');
                continue;
            }
            std::cout << "Available subdirectories:\n";
            for (size_t i = 0; i < subdirs.size(); ++i) {
                std::cout << (i + 1) << ") " << subdirs[i].filename().string() << "\n";
            }
            std::cout << "Select index (0=cancel): ";
            int sel;
            std::cin >> sel;
            if (!std::cin || sel <= 0 || sel > (int)subdirs.size()) {
                std::cin.clear();
                std::cin.ignore(10000, '\n');
                std::cout << "Cancelled.\n";
            }
            else {
                auto chosenDir = subdirs[sel - 1];
                std::vector<std::string> pyRoots;
                findPythonRoots(chosenDir, pyRoots);
                if (!pyRoots.empty()) {
                    hasPythonRoot = true;
                    chosenPythonRoot = pyRoots.front(); // take the first "root" found
                    std::cout << "Python 'root' found at: " << chosenPythonRoot << "\n";
                }
                else {
                    hasPythonRoot = false;
                    chosenPythonRoot.clear();
                    std::cout << "No 'root' folder found in that directory.\n";
                }
            }
            std::cout << "Press ENTER to continue...\n";
            std::cin.ignore(10000, '\n');
            continue;
        }
        else {
            // invalid choice
            continue;
        }

        // If we get here => configChoice == 4 => main menu
        while (true) {
            clearConsole();
            std::cout << "==============================\n";
            std::cout << "        M A I N   M E N U     \n";
            std::cout << "==============================\n\n";

            // color-coded for availability
            if (hasClientHeader) setColor(10); else setColor(12);
            std::cout << "1) Client\n";

            if (hasServerHeader) setColor(10); else setColor(12);
            std::cout << "2) Server\n";

            if (hasPythonRoot) setColor(10); else setColor(12);
            std::cout << "3) Python\n";

            setColor(7);
            std::cout << "4) Back to Path Settings\n";
            std::cout << "0) Exit\n";
            std::cout << "Choice: ";

            int choice;
            std::cin >> choice;
            if (!std::cin) {
                std::cin.clear();
                std::cin.ignore(10000, '\n');
                continue;
            }
            std::cin.ignore(10000, '\n');

            if (choice == 0) {
                std::cout << "Exiting.\n";
                return 0;
            }
            else if (choice == 4) {
                // back to path selection
                break;
            }
            else if (choice == 1) {
                // Client
                clearConsole();
                if (!hasClientHeader) {
                    std::cerr << "No client header found. Please set Client Path first.\n";
                    std::cout << "Press ENTER...\n";
                    std::cin.ignore(10000, '\n');
                    continue;
                }
                auto defines = readDefines(clientHeaderName);
                if (defines.empty()) {
                    std::cerr << "No #define entries in " << clientHeaderName << ".\n";
                    std::cout << "Press ENTER...\n";
                    std::cin.ignore(10000, '\n');
                    continue;
                }
                auto sourceFiles = findSourceFiles(clientPath);
                if (sourceFiles.empty()) {
                    std::cerr << "No .cpp/.h files found in " << clientPath << ".\n";
                    std::cout << "Press ENTER...\n";
                    std::cin.ignore(10000, '\n');
                    continue;
                }
                while (true) {
                    clearConsole();
                    std::cout << "CLIENT defines in " << clientHeaderName << ":\n";
                    for (size_t i = 0; i < defines.size(); ++i) {
                        std::cout << (i + 1) << ") " << defines[i] << "\n";
                    }
                    std::cout << "0) Back\nChoice: ";
                    int dchoice;
                    std::cin >> dchoice;
                    std::cin.ignore(10000, '\n');
                    if (!std::cin || dchoice == 0) {
                        break;
                    }
                    if (dchoice < 1 || dchoice >(int)defines.size()) {
                        std::cerr << "Invalid choice!\n";
                        continue;
                    }
                    std::string def = defines[dchoice - 1];
                    auto results = parseAllFilesMultiThread(sourceFiles, def);

                    fs::create_directory("Output");
                    {
                        std::ostringstream fname;
                        fname << "Output/CLIENT_" << def << "_DEFINE.txt";
                        std::ofstream outDef(fname.str());
                        std::unordered_set<std::string> defFiles;
                        for (auto& b : results.first) {
                            outDef << b.content << "\n";
                            defFiles.insert(b.filename);
                        }
                        outDef << "\n--- SUMMARY (" << results.first.size()
                            << " DEFINE block(s)) in files: ---\n";
                        for (auto& fn : defFiles) {
                            outDef << fn << "\n";
                        }
                    }
                    {
                        std::ostringstream fname;
                        fname << "Output/CLIENT_" << def << "_FUNC.txt";
                        std::ofstream outFunc(fname.str());
                        std::unordered_set<std::string> funcFiles;
                        for (auto& b : results.second) {
                            outFunc << b.content << "\n";
                            funcFiles.insert(b.filename);
                        }
                        outFunc << "\n--- SUMMARY (" << results.second.size()
                            << " function block(s)) in files: ---\n";
                        for (auto& fn : funcFiles) {
                            outFunc << fn << "\n";
                        }
                    }
                    setColor(10);
                    std::cout << "Done for define '" << def << "'. Press ENTER...\n";
                    setColor(7);
                    std::cin.ignore(10000, '\n');
                }
            }
            else if (choice == 2) {
                // Server
                clearConsole();
                if (!hasServerHeader) {
                    std::cerr << "No server header found. Please set Server Path first.\n";
                    std::cout << "Press ENTER...\n";
                    std::cin.ignore(10000, '\n');
                    continue;
                }
                auto defines = readDefines(serverHeaderName);
                if (defines.empty()) {
                    std::cerr << "No #define entries in " << serverHeaderName << ".\n";
                    std::cout << "Press ENTER...\n";
                    std::cin.ignore(10000, '\n');
                    continue;
                }
                auto sourceFiles = findSourceFiles(serverPath);
                if (sourceFiles.empty()) {
                    std::cerr << "No .cpp/.h files found in " << serverPath << ".\n";
                    std::cout << "Press ENTER...\n";
                    std::cin.ignore(10000, '\n');
                    continue;
                }
                while (true) {
                    clearConsole();
                    std::cout << "SERVER defines in " << serverHeaderName << ":\n";
                    for (size_t i = 0; i < defines.size(); ++i) {
                        std::cout << (i + 1) << ") " << defines[i] << "\n";
                    }
                    std::cout << "0) Back\nChoice: ";
                    int dchoice;
                    std::cin >> dchoice;
                    std::cin.ignore(10000, '\n');
                    if (!std::cin || dchoice == 0) {
                        break;
                    }
                    if (dchoice < 1 || dchoice >(int)defines.size()) {
                        std::cerr << "Invalid choice!\n";
                        continue;
                    }
                    std::string def = defines[dchoice - 1];
                    auto results = parseAllFilesMultiThread(sourceFiles, def);

                    fs::create_directory("Output");
                    {
                        std::ostringstream fname;
                        fname << "Output/SERVER_" << def << "_DEFINE.txt";
                        std::ofstream outDef(fname.str());
                        std::unordered_set<std::string> defFiles;
                        for (auto& b : results.first) {
                            outDef << b.content << "\n";
                            defFiles.insert(b.filename);
                        }
                        outDef << "\n--- SUMMARY (" << results.first.size()
                            << " DEFINE block(s)) in files: ---\n";
                        for (auto& fn : defFiles) {
                            outDef << fn << "\n";
                        }
                    }
                    {
                        std::ostringstream fname;
                        fname << "Output/SERVER_" << def << "_FUNC.txt";
                        std::ofstream outFunc(fname.str());
                        std::unordered_set<std::string> funcFiles;
                        for (auto& b : results.second) {
                            outFunc << b.content << "\n";
                            funcFiles.insert(b.filename);
                        }
                        outFunc << "\n--- SUMMARY (" << results.second.size()
                            << " function block(s)) in files: ---\n";
                        for (auto& fn : funcFiles) {
                            outFunc << fn << "\n";
                        }
                    }
                    setColor(10);
                    std::cout << "Done for define '" << def << "'. Press ENTER...\n";
                    setColor(7);
                    std::cin.ignore(10000, '\n');
                }
            }
            else if (choice == 3) {
                // Python
                clearConsole();
                if (!hasPythonRoot) {
                    std::cerr << "No Python root set. Please set Python Root first.\n";
                    std::cout << "Press ENTER...\n";
                    std::cin.ignore(10000, '\n');
                    continue;
                }
                // gather all .py files
                std::vector<std::string> pyFiles;
                try {
                    for (auto& p : fs::recursive_directory_iterator(chosenPythonRoot,
                        fs::directory_options::skip_permission_denied))
                    {
                        if (fs::is_symlink(p.path())) continue;
                        if (fs::is_regular_file(p.path()) && p.path().extension() == ".py") {
                            pyFiles.push_back(p.path().string());
                        }
                    }
                }
                catch (...) {}
                if (pyFiles.empty()) {
                    std::cerr << "No .py files found in " << chosenPythonRoot << ".\n";
                    std::cout << "Press ENTER...\n";
                    std::cin.ignore(10000, '\n');
                    continue;
                }
                // gather all possible "app.xyz" parameters
                auto paramSet = collectPythonParameters(pyFiles);
                if (paramSet.empty()) {
                    std::cerr << "No 'if app.xyz' lines found in that root.\n";
                    std::cout << "Press ENTER...\n";
                    std::cin.ignore(10000, '\n');
                    continue;
                }
                std::vector<std::string> params(paramSet.begin(), paramSet.end());
                std::sort(params.begin(), params.end());

                while (true) {
                    clearConsole();
                    std::cout << "Python app.<param> found:\n";
                    for (size_t i = 0; i < params.size(); ++i) {
                        std::cout << (i + 1) << ") " << params[i] << "\n";
                    }
                    std::cout << "0) Back\nChoice: ";
                    int pchoice;
                    std::cin >> pchoice;
                    std::cin.ignore(10000, '\n');
                    if (!std::cin || pchoice == 0) {
                        break;
                    }
                    if (pchoice < 1 || pchoice >(int)params.size()) {
                        std::cerr << "Invalid choice!\n";
                        continue;
                    }
                    std::string chosenParam = params[pchoice - 1];
                    auto pyResults = parsePythonAllFilesMultiThread(pyFiles, chosenParam);

                    fs::create_directory("Output");
                    {
                        std::ostringstream fname;
                        fname << "Output/PYTHON_" << chosenParam << "_DEFINE.txt";
                        std::ofstream out(fname.str());
                        std::unordered_set<std::string> defFiles;
                        for (auto& b : pyResults.first) {
                            out << b.content << "\n";
                            defFiles.insert(b.filename);
                        }
                        out << "\n--- SUMMARY (" << pyResults.first.size()
                            << " if-block(s)) in files: ---\n";
                        for (auto& fn : defFiles) {
                            out << fn << "\n";
                        }
                    }
                    {
                        std::ostringstream fname;
                        fname << "Output/PYTHON_" << chosenParam << "_FUNC.txt";
                        std::ofstream out(fname.str());
                        std::unordered_set<std::string> funcFiles;
                        for (auto& b : pyResults.second) {
                            out << b.content << "\n";
                            funcFiles.insert(b.filename);
                        }
                        out << "\n--- SUMMARY (" << pyResults.second.size()
                            << " function block(s)) in files: ---\n";
                        for (auto& fn : funcFiles) {
                            out << fn << "\n";
                        }
                    }
                    setColor(10);
                    std::cout << "Done for app." << chosenParam << ". Press ENTER...\n";
                    setColor(7);
                    std::cin.ignore(10000, '\n');
                }
            }
            else {
                // invalid
                continue;
            }
        } // end of main menu loop
    } // end of path config loop
}

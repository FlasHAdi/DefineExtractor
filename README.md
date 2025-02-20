# README

## English Version

### What does this tool do?

This tool scans through C++ and Python files in a given directory structure. It analyzes:
- C++ code for `#if/#ifdef/#ifndef` blocks that match specific `#define` macros
- C++ function definitions potentially affected by these defines
- Python code for `if app.<param>` statements
- Python functions containing these `if app.<param>` conditions

It then outputs all the discovered code blocks (both conditional blocks and functions) into separate text files for easy review.

### Overview

1. **C++ Parsing**  
   - Searches for specific `#define` names in two particular header files (for example, `locale_inc.h` and `service.h/commondefines.h`).  
   - Recursively scans through `.cpp` and `.h` files.  
   - Collects all code blocks wrapped in `#if / #ifdef / #ifndef` that match the chosen macro.  
   - Detects and extracts functions that contain or are influenced by these macros.

2. **Python Parsing**  
   - Looks for lines containing `if app.<param>` patterns.  
   - Gathers the Python functions that contain these conditions.  

3. **Output Generation**  
   - For each chosen `#define` (in C++) or Python parameter, two files are created under an `Output/` folder:  
     - One for conditional blocks (`*_DEFINE.txt`).  
     - One for functions (`*_FUNC.txt`).  
   - Each file includes a summary listing the source files where blocks were found.

### How it Works

1. **Parameter Discovery**  
   - For C++, the tool reads all available `#define` names from the specified header.  
   - For Python, it searches `if app.xyz` lines across Python files to collect possible parameter names (`xyz`).  

2. **Multi-Threaded Parsing**  
   - The tool counts lines first (using a cache to avoid redundant reads).  
   - It then creates multiple threads (up to the number of hardware concurrency or the number of files, whichever is smaller).  
   - Each thread processes a subset of the files in parallel, ensuring quick scanning on large codebases.  

3. **Block Extraction**  
   - Whenever a `#if/<if` condition matches the chosen define or Python parameter, the tool accumulates all lines until the end of that block.  
   - Functions are recognized by pattern matching on function signatures (C++: via a regex for possible function heads; Python: via `def` statements).  
   - If a function contains any matching condition, it’s marked as "relevant" and extracted.

4. **Progress Indication**  
   - While running, the tool shows a progress bar in the console, updating at regular intervals as files are parsed.

### Visual Studio Settings to Build the Tool

1. **Project Creation**  
   - Create a new **Console Application** in Visual Studio (C++).  
   - Ensure C++17 (or newer) is enabled, as `<filesystem>` is used.  

2. **Additional Dependencies**  
   - The code uses `<regex>`, `<thread>`, `<mutex>`, `<atomic>`, `<filesystem>`. These should be available in C++17+ by default.  

3. **Character Set and Warnings**  
   - You might want to set the project to **Multi-Byte** or **Unicode**. The code primarily uses standard string functions, so no special requirement here.  
   - Disable or fix any warning levels as needed.  

4. **Include and Link**  
   - No extra libraries are required beyond the standard C++ library.  

### Compiling and Running

1. **Compiling**  
   - Load the solution in Visual Studio.  
   - Ensure the configuration is set to **Release** (for best performance) or **Debug** if you want to step through code.  
   - Click **Build** -> **Build Solution**.

2. **Running**  
   - Place the compiled executable in a directory from which it can recursively scan the code. Usually, you can run it from the project folder (where the `.cpp` files are).  
   - The tool will automatically look for `locale_inc.h`, `service.h`, or `commondefines.h` in the directory tree, as well as a folder named `root` for Python files.  
   - Follow the on-screen menu instructions to choose your define or Python parameter.  

### Performance

- **Line Counting**  
  - The tool maintains a cache so each file's line count is determined only once. This reduces unnecessary repeated file reads.

- **Multi-Threading**  
  - By dividing the parsing across multiple threads, the tool can handle even large codebases efficiently, often limited by the speed of your disk and CPU cores.

- **Progress Bar**  
  - Displays real-time progress. Parsing speed is typically fast enough that the progress bar will be quite responsive, though it only refreshes at small intervals to avoid overhead.

---

## Deutsche Version

### Was macht dieses Tool?

Dieses Tool durchforstet C++- und Python-Dateien in einer angegebenen Verzeichnisstruktur. Es analysiert:
- C++-Code nach `#if/#ifdef/#ifndef`-Blöcken, die zu bestimmten `#define`-Makros passen
- C++-Funktionsdefinitionen, die möglicherweise von diesen Defines betroffen sind
- Python-Code nach Zeilen `if app.<param>`
- Python-Funktionen, welche diese `if app.<param>`-Bedingungen enthalten

Alle gefundenen Code-Blöcke (sowohl Bedingungsblöcke als auch Funktionsblöcke) werden in separate Textdateien ausgegeben, damit sie leicht überprüft werden können.

### Übersicht

1. **C++-Parsing**  
   - Sucht bestimmte `#define`-Namen in zwei speziellen Headerdateien (z.B. `locale_inc.h` und `service.h/commondefines.h`).  
   - Durchsucht rekursiv `.cpp`- und `.h`-Dateien.  
   - Sammelt alle Code-Blöcke, die in `#if / #ifdef / #ifndef`-Bedingungen liegen, welche das ausgewählte Makro betreffen.  
   - Ermittelt und extrahiert alle Funktionen, die diese Defines enthalten oder von ihnen beeinflusst werden.

2. **Python-Parsing**  
   - Sucht nach Zeilen, die `if app.<param>` enthalten.  
   - Identifiziert die zugehörigen Python-Funktionen, falls innerhalb derselben Funktionsdefinition.  

3. **Ausgabe**  
   - Für jeden ausgewählten `#define` (C++) oder Python-Parameter werden unter dem Ordner `Output/` jeweils zwei Dateien erzeugt:  
     - Eine für die Bedingungsblöcke (`*_DEFINE.txt`).  
     - Eine für die Funktionsblöcke (`*_FUNC.txt`).  
   - Jede Datei enthält zudem eine Zusammenfassung, in welchen Dateien Blöcke gefunden wurden.

### Funktionsweise

1. **Ermitteln der Parameter**  
   - Für C++ liest das Tool alle vorhandenen `#define`-Namen aus der angegebenen Headerdatei.  
   - Für Python durchsucht es sämtliche Python-Dateien nach Zeilen der Form `if app.xyz`, um mögliche Parameter (`xyz`) zu sammeln.  

2. **Multi-Threaded Parsing**  
   - Das Tool ermittelt zunächst die Zeilenanzahl aller relevanten Dateien (unter Nutzung eines Caches, um Mehrfachzugriffe zu vermeiden).  
   - Anschließend werden mehrere Threads (bis zur Anzahl der CPU-Kerne oder der verfügbaren Dateien) gestartet.  
   - Jeder Thread bearbeitet einen Teil der Dateien parallel, was für eine schnelle Verarbeitung auch in größeren Codebasen sorgt.

3. **Block-Extraktion**  
   - Bei einem Fund von `#if/<if`-Bedingungen mit dem gesuchten Macro oder Python-Parameter, sammelt das Tool den gesamten Codeblock bis zum Ende.  
   - Funktionen werden per Regex (C++: mögliche Funktionssignaturen, Python: `def`) erkannt.  
   - Enthält eine Funktion einen relevanten Bedingungsblock, wird sie markiert und in die Ausgabe übernommen.

4. **Fortschrittsanzeige**  
   - Während der Laufzeit wird in der Konsole ein Fortschrittsbalken angezeigt, der sich in kurzen Abständen aktualisiert.

### Visual-Studio-Einstellungen zum Erstellen des Tools

1. **Projekt anlegen**  
   - Erstellen Sie eine neue **Konsolenanwendung** in Visual Studio (C++).  
   - Aktivieren Sie C++17 (oder höher), da `<filesystem>` genutzt wird.  

2. **Zusätzliche Abhängigkeiten**  
   - Das Programm verwendet `<regex>`, `<thread>`, `<mutex>`, `<atomic>`, `<filesystem>`. Diese sind in C++17+ standardmäßig enthalten.  

3. **Zeichensatz und Warnungen**  
   - Ggf. Projekt auf **Multi-Byte** oder **Unicode** einstellen. Das Programm verwendet Standard-String-Funktionen, daher gibt es hier keine besonderen Anforderungen.  
   - Warnlevel nach Bedarf anpassen oder beheben.  

4. **Include und Linker**  
   - Es werden keine weiteren Bibliotheken benötigt, nur die Standard-C++-Bibliothek.  

### Kompilieren und Ausführen

1. **Kompilieren**  
   - Öffnen Sie die Lösung in Visual Studio.  
   - Stellen Sie sicher, dass die Konfiguration auf **Release** (für optimale Performance) oder **Debug** (zum Debuggen) gesetzt ist.  
   - Wählen Sie **Build** -> **Build Solution** aus.

2. **Ausführen**  
   - Kopieren Sie die erzeugte ausführbare Datei in ein Verzeichnis, in dem das Tool rekursiv nach Code-Dateien suchen kann. Häufig ist das direkt der Projektordner.  
   - Das Tool sucht automatisch nach `locale_inc.h`, `service.h` oder `commondefines.h` sowie nach einem Ordner namens `root` für Python-Dateien.  
   - Befolgen Sie die Menüanweisungen in der Konsole, um Ihren gewünschten `#define` oder Python-Parameter auszuwählen.  

### Performance

- **Zeilenanzahl-Caching**  
  - Die Zeilenanzahl jeder Datei wird nur einmal ermittelt und zwischengespeichert, um unnötige Wiederholungen beim Zugriff auf dieselbe Datei zu vermeiden.

- **Multi-Threading**  
  - Durch die Aufteilung des Parsings auf mehrere Threads kann das Tool auch große Codebestände effizient durchsuchen. Die Geschwindigkeit hängt vor allem von der CPU- und Festplattenleistung ab.

- **Fortschrittsbalken**  
  - Der Fortschritt wird während des Einlesens angezeigt. Üblicherweise ist das Einlesen schnell genug, sodass die Aktualisierung regelmäßig (alle paar hundert Zeilen) erfolgt.


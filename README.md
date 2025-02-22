# DefineExtractor

[**Deutsch**](#deutsch) | [**English**](#english)

---

## <a name="deutsch"></a>Deutsch

### 1. Überblick

**DefineExtractor** ist ein C++-Kommandozeilenprogramm für Windows, das Quellcode nach folgenden Mustern durchsucht:
- `#define`-Makros in `.h`- und `.cpp`-Dateien
- `app.xyz`-Aufrufe in `.py`-Dateien

Das Tool unterstützt Mehrfach-Threads und kann große Codebasen effizient durchsuchen. Es hilft dabei, schnell die relevanten Stellen für bestimmte Defines oder Python-Parameter zu finden.

---

### 2. Hauptfunktionen

1. **Automatische Header-Erkennung**  
   - Findet `locale_inc.h` (Client) sowie `service.h` / `commondefines.h` (Server) durch Durchsuchen einschlägiger Unterordner.

2. **Makro-Listing**  
   - Zeigt alle definierten Makros aus den erkannten Headern an, um gezielt nach einem bestimmten `#define` zu suchen.

3. **Python-Parameter**  
   - Ermittelt alle Aufrufe im Format `app.xyz` innerhalb von `.py`-Dateien und listet diese übersichtlich auf.

4. **Ausgabedateien in `Output/`**  
   - Für jedes gescannte Makro bzw. jeden Python-Parameter erzeugt das Tool zwei Textdateien:  
     - `*_DEFINE.txt`: Fundstellen für `#if <DEFINE>`-Blöcke bzw. `if app.xyz`  
     - `*_FUNC.txt`: Funktionen oder Methoden, in denen das Define bzw. der Parameter auftaucht

5. **Multithreading**  
   - Dank gleichzeitiger Verarbeitung mehrerer Dateien kann die Suche in großen Projekten deutlich beschleunigt werden.

---

### 3. Performance & Ablauf

- **Parallele Verarbeitung**: Das Tool verteilt die zu durchsuchenden Dateien auf mehrere Threads (abhängig von der CPU-Anzahl).
- **Regex-gestütztes Parsing**: `#if`-Blöcke, Funktionsköpfe sowie Python-`if`-Statements werden über reguläre Ausdrücke erkannt. Dies funktioniert in den meisten konventionellen Code-Stilen zuverlässig.
- **Statusanzeige**: Während der Suche wird eine Fortschrittsleiste im Terminal angezeigt, die den aktuellen Fortschritt (in %) darstellt.
- **Ergebnisstruktur**: Pro Suchlauf entstehen zwei Kategorien von Ausgaben (für Blöcke und für Funktionen). Ein Überblick der betroffenen Dateien wird am Ende jeder Ausgabedatei angehängt.

---

### 4. Verzeichnisstruktur & Programmstart

1. **Programmdatei**  
   - Nach erfolgreichem Build (siehe [HowTo.md](HowTo.md)) erhältst du eine ausführbare Datei (z.B. `DefineExtractor.exe`).
   - Platziere sie in einer für dich sinnvollen Ordnerstruktur, in der du Zugriff auf deine Client-, Server- oder Python-Verzeichnisse hast.

2. **Client-, Server-, Python-Verzeichnisse**  
   - Idealerweise liegen diese Ordner auf derselben Ebene wie das Build-Verzeichnis:
     ```
     C:\MeineProjekte\
       ├─ DefineExtractor\ (enthält EXE)
       ├─ MeinClient\
       ├─ MeinServer\
       └─ PythonStuff\
     ```
   - So kann das Tool leicht die relevanten Pfade durchsuchen.

3. **Ausführung**  
   - Starte die Anwendung per Doppelklick oder über eine Kommandozeile (CMD/PowerShell).
   - Wähle zunächst in einem Menü die Pfade für **Client**, **Server** und **Python Root**.
   - Anschließend im **Hauptmenü** die gewünschte Option (Client/Server/Python) auswählen und ein Makro bzw. einen Parameter scannen.

---

### 5. Bekannte Einschränkungen

- **Regex-Grenzen**  
  Bei sehr unkonventionellen Code-Stilen (z.B. stark verschachtelte Makros) kann die Erkennung fehlschlagen oder versehentlich zu viel mit erfassen.
- **Nur Windows optimiert**  
  Zwar basiert das Projekt weitgehend auf C++17 und könnte unter Linux kompiliert werden, jedoch ist das Hauptaugenmerk auf Windows/Visual Studio gerichtet.
- **Keine tiefe Python-Analyse**  
  Die Python-Suche beschränkt sich auf `if app.xyz`-Blöcke und deren Einrückung sowie Funktionsdefinitionen (`def`). Komplexere Strukturen (z.B. Klassen) werden nicht gesondert behandelt.

---

### 6. Lizenz & Haftung

- Dieses Projekt enthält **keine spezifische Lizenz**. Gerne kannst du selbst eine hinzufügen (z.B. MIT, Apache 2.0 etc.).
- Die Nutzung erfolgt **auf eigene Verantwortung**. Es wird keine Haftung für mögliche Schäden oder Datenverluste übernommen.

---

### 7. Kontakt & Support

- **Bug Reports & Feature Requests**: Bitte im zugehörigen Repository ein **Issue** erstellen.
- **Pull Requests** sind immer willkommen.
- **Weitere Fragen**: Siehe [HowTo.md](HOWTO.md) für Einrichtungsdetails oder kontaktiere den Autor direkt.

---

## <a name="english"></a>English

### 1. Overview

**DefineExtractor** is a Windows-oriented C++ command-line tool that searches source code for:
- `#define` macros in `.h` and `.cpp` files
- `app.xyz` references in `.py` files

It employs multithreading for faster scanning of large codebases, making it easy to locate relevant lines or blocks tied to specific features (defines or Python parameters).

---

### 2. Key Features

1. **Automatic Header Detection**  
   - Finds `locale_inc.h` (Client) and `service.h` / `commondefines.h` (Server) by scanning typical subfolders.

2. **Macro Listing**  
   - Displays all macros defined in the discovered headers, allowing targeted searches for a particular `#define`.

3. **Python Parameter Discovery**  
   - Looks for `app.xyz` calls within `.py` files, listing them systematically.

4. **Output Files in `Output/`**  
   - For each scanned macro or Python parameter, the tool produces two text files:
     - `*_DEFINE.txt`: Contains relevant `#if <DEFINE>` or `if app.xyz` blocks
     - `*_FUNC.txt`: Contains functions/methods referencing that define or parameter

5. **Multithreading**  
   - Uses multiple threads to quickly process large file sets on multi-core CPUs.

---

### 3. Performance & Workflow

- **Parallel File Processing**: Distributes work across available CPU cores (thread count typically matches hardware concurrency).
- **Regex-Based Parsing**: Identifies `#if` blocks, function declarations, and Python `if app.xyz` statements via regular expressions.
- **Progress Display**: A progress bar in the console shows the scanning progress in real time.
- **Result Structure**: Each search yields two categories of output (blocks vs. functions). A summary of affected files is appended at the end of each output file.

---

### 4. Folder Layout & Running the Program

1. **Executable**  
   - After a successful build (see [HowTo.md](HOWTO.md)), you’ll have `DefineExtractor.exe`.
   - Place it in a convenient directory that can access your Client, Server, or Python source folders.

2. **Client, Server, and Python Folders**  
   - Typically, these folders sit in the same root as the build directory:
     ```
     C:\MyProjects\
       ├─ DefineExtractor\ (contains the .exe)
       ├─ MyClient\
       ├─ MyServer\
       └─ PythonStuff\
     ```
   - This layout makes scanning easier.

3. **Execution**  
   - Double-click the .exe or run it from a command prompt (CMD/PowerShell).
   - Choose your **Client** path, **Server** path, and **Python Root** in the path setup menu.
   - Then, from the **main menu**, pick Client/Server/Python and select a macro or parameter to scan.

---

### 5. Known Limitations

- **Regex Boundaries**  
  With highly unconventional or macro-heavy code, there is a risk of missing or over-including certain lines.
- **Windows Focus**  
  Although it uses standard C++17, the primary focus is Windows + Visual Studio.
- **Limited Python Analysis**  
  The Python search only targets `if app.xyz` blocks (with indentation) and function definitions (`def`). It doesn’t handle classes or more complex structures in detail.

---

### 6. License & Disclaimer

- **No specific license** is included by default. Feel free to add one (e.g., MIT, Apache 2.0).
- **Use at your own risk**. We assume no liability for any damage or data loss.

---

### 7. Contact & Support

- **Bug Reports & Feature Requests**: Please file an **Issue** in the repository.
- **Pull Requests** are welcome.
- **Further Questions**: Refer to [HowTo.md](HowTo.md) for setup instructions or contact the author directly.

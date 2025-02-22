# HowTo – Windows & Visual Studio Setup

[**Deutsch**](#deutsch) | [**English**](#english)

---

## <a name="deutsch"></a>Deutsch

### 1. Voraussetzungen

- **Microsoft Visual Studio 2019** (oder neuer)  
  Achte bei der Installation darauf, dass die **C++-Entwicklungsumgebung** (z.B. "Desktopentwicklung mit C++") enthalten ist.
- **DefineExtractor-Quellcode**  
  Platziere `main.cpp` (und ggf. weitere Quellcodedateien) in einem Ordner, z.B. `C:\MeineProjekte\DefineExtractor\`.

### 2. Neues Projekt in Visual Studio

1. **Leeres C++-Projekt erstellen**  
   - Starte Visual Studio.  
   - Klicke auf **"Create a new project"** → wähle **"Empty Project"** (oder einen vergleichbaren Typ für ein C++-Konsolenprogramm).  
   - Vergib einen Projekt-Namen (z.B. "DefineExtractor").

2. **Quellcode hinzufügen**  
   - Im **Solution Explorer** (Strg + Alt + L) rechtsklick auf dein Projekt.  
   - Wähle **"Add" → "Existing Item"**.  
   - Füge deine Quellcodedatei(en) hinzu (z.B. `main.cpp`).

3. **C++17 aktivieren**  
   - Rechtsklick auf dein Projekt → **Properties** (Eigenschaften).  
   - Unter **Configuration Properties** → **C/C++** → **Language** sicherstellen, dass das Sprachlevel auf **C++17** (z.B. `/std:c++17`) eingestellt ist.

### 3. Kompilieren & Ausführen

1. **Build-Konfiguration**  
   - Oben in der Visual-Studio-Symbolleiste kannst du zwischen "Debug" und "Release" wählen.  
   - "Release" erzeugt in der Regel eine optimierte und kleinere EXE.

2. **Build starten**  
   - Menü **Build** → **Build Solution** (oder Strg+Umschalt+B).  
   - Bei Erfolg siehst du im Ausgabefenster "Build: 1 succeeded, 0 failed".

3. **Programm starten**  
   - **Debug** → **Start without Debugging** (Strg+F5).  
   - Ein Konsolenfenster öffnet sich und zeigt das Hauptmenü des **DefineExtractors**.

### 4. Ordnerstruktur & Anwendung

1. **Auffinden der EXE**  
   - Nach dem erfolgreichen Build liegt die ausführbare Datei (z.B. `DefineExtractor.exe`) meist unter  
     `C:\MeineProjekte\DefineExtractor\x64\Release\`  
     (oder im `Debug\`-Verzeichnis, wenn du im Debug-Modus gebaut hast).

2. **Client-, Server-, und Python-Verzeichnisse**  
   - Lege deine **Client**-, **Server**- und/oder **Python**-Ordner am besten auf derselben Ebene ab, damit das Programm beim Pfadauswählen problemlos darauf zugreifen kann.  
   - Beispielstruktur:  
     ```
     C:\MeineProjekte\
       ├─ DefineExtractor\ (enthält EXE und VS-Projekt)
       ├─ MeinClient\
       ├─ MeinServer\
       └─ PythonStuff\
     ```

3. **DefineExtractor starten**  
   - Doppelklicke die EXE oder rufe sie in der Eingabeaufforderung / PowerShell auf.  
   - Zuerst wählst du im **Pfad-Menü** den Ordner für:
     - **Client** (`locale_inc.h` in "UserInterface" o.ä.)
     - **Server** (`service.h` / `commondefines.h` in "common" o.ä.)
     - **Python Root** (ein Unterverzeichnis namens `root` mit `.py`-Dateien)
   - Im **Hauptmenü** kannst du dann:
     - **Client** oder **Server** nach Makros durchsuchen,  
     - **Python** nach `app.xyz`-Parametern durchsuchen.  

4. **Suchergebnisse**  
   - Die relevanten Code-Stellen werden rekursiv in `.cpp`, `.h` (für Client/Server) oder `.py` (für Python) gesucht.  
   - Für jedes Makro bzw. jeden Parameter erzeugt das Tool zwei Textdateien im Ordner `Output/`:
     - `*_DEFINE.txt`: Enthält die `#if ... #endif`-Blöcke oder `if app.xyz`-Blöcke.  
     - `*_FUNC.txt`: Enthält alle Funktionen, in denen das entsprechende Makro oder der Parameter auftritt.

### 5. Tipps & Fehlersuche

- **Header nicht gefunden**  
  - Prüfe, ob `locale_inc.h` (Client) bzw. `service.h/commondefines.h` (Server) wirklich in den vom Tool gesuchten Unterordnern liegen. 
- **Keine Python-Dateien**  
  - Das Tool sucht nach einem Unterverzeichnis namens `root`. Achte auf korrekte Schreibweise.  
- **Leer bleibender `Output/`-Ordner**  
  - Möglicherweise gab es keine Treffer oder das Tool hatte keine Leseberechtigungen.  
- **Regex-Anpassung**  
  - Bei sehr exotischem Code-Stil kann es nötig sein, die Regex in `main.cpp` (z.B. `functionHeadRegex`) anzupassen.

---

## <a name="english"></a>English

### 1. Prerequisites

- **Microsoft Visual Studio 2019** (or newer)  
  Make sure the "Desktop development with C++" workload is installed.
- **DefineExtractor source files**  
  Place `main.cpp` (and any additional files) in a folder, e.g. `C:\MyProjects\DefineExtractor\`.

### 2. Creating a New Project in Visual Studio

1. **Empty C++ Project**  
   - Launch Visual Studio.  
   - Click **"Create a new project"** → choose **"Empty Project"**.  
   - Name it (e.g. "DefineExtractor").

2. **Add Source Code**  
   - In the **Solution Explorer** (Ctrl + Alt + L), right-click your project.  
   - Select **"Add" → "Existing Item"**.  
   - Add your source file(s) (e.g. `main.cpp`).

3. **Enable C++17**  
   - Right-click your project → **Properties**.  
   - Under **Configuration Properties** → **C/C++** → **Language**, set `C++17` (e.g. `/std:c++17`).

### 3. Building & Running

1. **Build Configuration**  
   - At the top in Visual Studio, pick either "Debug" or "Release".  
   - "Release" is typically faster and produces a smaller executable.

2. **Build**  
   - Go to **Build** → **Build Solution** (or press Ctrl+Shift+B).  
   - If successful, the Output panel shows "Build: 1 succeeded, 0 failed".

3. **Run**  
   - **Debug** → **Start without Debugging** (Ctrl+F5).  
   - A console window appears, displaying the DefineExtractor main menu.

### 4. Folder Structure & Usage

1. **Locate the EXE**  
   - After building, you’ll find `DefineExtractor.exe` typically in  
     `C:\MyProjects\DefineExtractor\x64\Release\`  
     (or in `Debug\` if in Debug mode).

2. **Client, Server, and Python Folders**  
   - For best results, place your **Client**, **Server**, and/or **Python** folders parallel to the DefineExtractor directory.  
   - Example:  
     ```
     C:\MyProjects\
       ├─ DefineExtractor\ (VS project & .exe)
       ├─ MyClient\
       ├─ MyServer\
       └─ PythonStuff\
     ```

3. **Running DefineExtractor**  
   - Double-click the executable or run it via Command Prompt / PowerShell.  
   - In the **Path menu**, specify:
     - **Client** (contains `locale_inc.h` in e.g. "UserInterface")
     - **Server** (contains `service.h` / `commondefines.h` in e.g. "common")
     - **Python Root** (a subfolder named `root` that has `.py` files)
   - Then use the **main menu** to:
     - Search **Client** or **Server** macros,  
     - or **Python** parameters (`app.xyz`).

4. **Results**  
   - The tool recursively scans `.cpp`, `.h` (for Client/Server) or `.py` (for Python) files.  
   - For each macro/parameter, two text files are generated in `Output/`:
     - `*_DEFINE.txt`: Contains `#if ... #endif` or `if app.xyz` blocks.  
     - `*_FUNC.txt`: Lists functions that reference the macro or parameter.

### 5. Tips & Troubleshooting

- **Header Not Detected**  
  - Verify that `locale_inc.h` (Client) and `service.h` / `commondefines.h` (Server) reside in the folders the tool expects to find them.
- **Missing Python Files**  
  - Confirm there is a `root` subfolder containing `.py` files.
- **No `Output/` Files**  
  - Possibly no matches were found, or file access was restricted.
- **Adjusting Regex**  
  - In unusual code styles, you may need to edit `functionHeadRegex` or other patterns in `main.cpp`.

---

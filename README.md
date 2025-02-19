# Define Parser

Dieses Projekt durchsucht rekursiv alle `.cpp`- und `.h`-Dateien im aktuellen Verzeichnis nach bestimmten Codeblöcken, die von ausgewählten `#define`-Macros eingeschlossen sind. Zusätzlich werden Funktionsdefinitionen extrahiert, sofern innerhalb dieser Funktionen Referenzen auf das gewählte `#define` vorhanden sind. Die Ergebnisse werden automatisch in separate Textdateien im Ordner `Output/` gespeichert.

**Languages / Sprachen:**
- [Deutsch](#deutsch)
- [English](#english)

---

## Deutsch

### Übersicht
Dieses C++-Programm durchsucht:
1. Zunächst eine Header-Datei, um potenzielle `#define`-Einträge zu finden.
2. Anschließend alle `.cpp`- und `.h`-Dateien nach Codeblöcken, die auf das jeweilige `#define` verweisen.
3. Es extrahiert sowohl die `#if(define)`-Blöcke als auch Funktionsblöcke, in denen ebenfalls das `#define` vorkommt.

Es bietet ein Konsolenmenü zur Auswahl zwischen Client- und Server-Header und präsentiert anschließend eine Liste aller gefundenen `#define`. Nach der Auswahl eines Eintrags werden alle passenden Codeausschnitte gesammelt und in zwei Dateien ausgegeben:
- `*_DEFINE.txt` für `#if(define)`-Blöcke
- `*_FUNC.txt` für Funktionsblöcke

### Funktionsweise
1. **Headerdatei finden**  
   Das Programm versucht, anhand bekannter Dateinamen (z. B. `locale_inc.h` für Client und `service.h`, `commondefines.h` für Server) eine passende Header-Datei zu finden.

2. **Ermittlung aller `#defines`**  
   In der gefundenen Header-Datei werden Zeilen gesucht, die `#define <NAME>` entsprechen. Diese Einträge werden im Programm aufgelistet.

3. **Durchsuchen des Projektverzeichnisses**  
   Alle `.cpp`- und `.h`-Dateien im aktuellen Verzeichnis (rekursiv) werden eingelesen.

4. **Suche nach Codeblöcken**  
   - **`#if(define)`-Blöcke**: Das Programm erkennt, wenn ein `#if`, `#ifdef` oder `#ifndef` (etc.) auf das ausgewählte `#define` verweist und zeichnet dabei den kompletten Block (inkl. eventuell verschachtelter `#if`/`#endif`).
   - **Funktionsblöcke**: Sobald innerhalb einer Funktion auf das ausgewählte `#define` verwiesen wird, zeichnet das Programm den gesamten Funktionsrumpf auf.

5. **Ausgabe**  
   - Für jeden ausgewählten `#define`-Eintrag werden zwei Ausgabedateien im Ordner `Output/` erzeugt:
     - `<PREFIX>_<DEFINE>_DEFINE.txt`
     - `<PREFIX>_<DEFINE>_FUNC.txt`
   - `<PREFIX>` ist abhängig davon, ob im Programm “Client” oder “Server” gewählt wurde (z. B. `CLIENT_`, `SERVER_`).

### Visual Studio Projektkonfiguration
- **C++-Standard**: Das Projekt sollte mit **C++17** (oder höher) kompiliert werden.  
  - In Visual Studio:  
    - Rechtsklick auf das Projekt → **Properties** (Eigenschaften)  
    - **Configuration Properties** → **C/C++** → **Language** → **C++ Language Standard** → z. B. `ISO C++17 Standard (/std:c++17)`
- **Weitere Einstellungen**:  
  - Falls das `std::filesystem`-Header in Ihrer Umgebung nicht voll verfügbar ist, verwenden Sie ggf. `<experimental/filesystem>` und aktivieren Sie die entsprechende Compiler-Option (`/std:c++17` oder `/std:c++latest`).
  - Achten Sie darauf, dass die Konsolenausgabe Unicode-fähig ist, falls Sie Umlaute oder Sonderzeichen ausgeben möchten (unter Windows ggf. `chcp 65001` nutzen).

### Kompilieren und Ausführen
1. **Projektdatei in Visual Studio öffnen** oder ein neues Projekt anlegen und den Code einfügen.
2. **C++ Standard** auf C++17 (oder neuer) setzen.
3. **Kompilieren** und **Starten** (F5 oder `Ctrl+F5` in Visual Studio).

Nach dem Start:
- Das Programm sucht automatisch nach einer passenden Header-Datei.
- Es fragt, ob Client (1) oder Server (2) ausgewählt werden soll.
- Findet es keine geeigneten Header-Dateien, bricht es ab.
- Bei Erfolg listet es alle gefundenen `#define`-Einträge auf.
- Nach Auswahl eines Eintrags führt es die Analyse durch.

### Performance
- Das Programm nutzt **Mehrthreading** (so viele Threads wie CPU-Kerne) für die Analyse der Dateien. Jede Datei wird in einem eigenen Chunk verarbeitet, was auf Mehrkernsystemen zu einer deutlichen Beschleunigung führt.
- Die **Zeilenzählung** erfolgt vorab (Single-Thread). Diese Informationen werden dann im laufenden Parsing genutzt, um einen Fortschrittsbalken anzuzeigen.
- Typische Performance:
  - Kleinere Projekte mit einigen Hundert Dateien werden in **Sekunden** bis wenigen **Dutzend Sekunden** analysiert.
  - Größere Codebasen mit tausenden Dateien und mehreren hunderttausend Zeilen Code können je nach Festplatte und CPU dennoch innerhalb von **einigen Sekunden bis wenigen Minuten** fertig sein.

---

## English

### Overview
This C++ program recursively scans `.cpp` and `.h` files in the current directory, looking for specific code blocks that reference a chosen `#define`. It also extracts function definitions that contain references to that `#define`. The results are automatically saved into separate text files in the `Output/` folder.

### How it works
1. **Find a header file**  
   The program attempts to locate a known header file (e.g. `locale_inc.h` for client, or `service.h`, `commondefines.h` for server) where `#define`s are expected.

2. **Collect `#define` entries**  
   It reads each line of the discovered header file to identify lines like `#define <NAME>`. These entries are then presented in a console menu.

3. **Scan project directory**  
   Every `.cpp` and `.h` file in the current directory (recursive) is examined.

4. **Search for code blocks**  
   - **`#if(define)` blocks**: The parser recognizes when a `#if`, `#ifdef`, or `#ifndef` (etc.) references the selected `#define`, capturing the entire block (including nested `#if`/`#endif` pairs).
   - **Function blocks**: Whenever a function contains references to the chosen `#define`, the entire function body is recorded.

5. **Output**  
   - For each selected `#define`, two output files are generated in the `Output/` folder:
     - `<PREFIX>_<DEFINE>_DEFINE.txt`
     - `<PREFIX>_<DEFINE>_FUNC.txt`
   - `<PREFIX>` is determined by whether the user chose “Client” or “Server” (e.g., `CLIENT_` or `SERVER_`).

---

## Lizenz & Kontakt
Dieses Projekt kann frei genutzt und angepasst werden. Bei Fragen oder Vorschlägen können Sie gerne ein Issue auf GitHub erstellen oder den Autor kontaktieren.


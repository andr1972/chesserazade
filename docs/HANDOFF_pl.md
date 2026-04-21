# Chesserazade — Handoff do implementacji

**Odbiorca:** programista (lub agent kodujący), który zrealizuje ten projekt od zera, wersja po wersji.
**Repozytorium:** `chesserazade` (obecnie puste, jedynie `README.md`, `LICENSE`, `.gitignore`).
**Język komunikacji z użytkownikiem:** polski. Język kodu, komentarzy, identyfikatorów, komunikatów commitów i wyjścia CLI: angielski.

---

## 1. Cel i filozofia

Chesserazade to **edukacyjny program szachowy**. Jego główną wartością jest to, że kod źródłowy stanowi czysty, klasyczny materiał referencyjny, który *można czytać i się z niego uczyć* — a nie jego siła gry.

Gwiazdy przewodnie projektu, w kolejności ważności:

1. **Czytelny kod źródłowy.** Ktoś otwierający dowolny plik `.cpp` / `.hpp` powinien rozumieć, co robi, dlaczego i jak wpasowuje się w całość. Nazwane typy, nazwane stałe, opisowe funkcje i przemyślane komentarze tam, gdzie *dlaczego* nie jest oczywiste. Ten projekt świadomie odchodzi od domyślnej zasady „bez komentarzy": przemyślane komentarze dokumentacyjne dla publicznych API i nietrywialnych algorytmów są *oczekiwane*.
2. **Klasyczne podejście.** Stosuj techniki, które opisałby podręcznik programowania szachowego: reprezentacja planszy mailbox, generowanie pseudo-legalnych ruchów + filtr legalności, make/unmake, minimax, alpha-beta pruning, Zobrist hashing, uporządkowanie ruchów (move ordering), quiescence search. Żadnych sieci neuronowych, żadnego NNUE, żadnych egzotycznych trików SIMD w linii 0.x–1.0.
3. **Modułowość.** Główne podsystemy (plansza, generator ruchów, funkcja oceny, szukanie, front-end protokołu/CLI, rozwiązywacz zadań, I/O PGN) są wydzielone za dobrze nazwanymi interfejsami, tak aby czytelnik mógł studiować je niezależnie, a w wersji 1.1 dało się wymienić planszę mailbox na bitboardową bez zmiany reszty.
4. **Poprawność przed szybkością.** Przechodź dokładnie standardowe pozycje perft. Optymalizacje dopiero *po* osiągnięciu poprawności.

### Czym ten projekt **nie jest**

- Nie jest silnikiem do konkurencji. Nie pokona Stockfisha, Leeli, Komodo ani żadnego nowoczesnego silnika. To jawny non-goal.
- Nie jest przede wszystkim narzędziem do gry człowieka z programem. Gra człowiek-człowiek i człowiek-komputer są drugorzędne.
- Nie jest platformą badawczą do nowatorskich algorytmów.

### Do czego ten projekt **służy**

Główne przypadki użycia, w kolejności ważności:

1. **Generowanie legalnych ruchów i perft** — wygenerowanie wszystkich legalnych ruchów do zadanej głębokości, zliczenie węzłów, pomiar czasu. Dostępne jako narzędzie CLI i jako biblioteka.
2. **Rozwiązywanie zadań** — dla danego FEN znaleźć mata w N (N ≥ 1), znaleźć motywy taktyczne, znaleźć najlepszy ruch na zadanej głębokości przeszukiwania.
3. **Analiza partii** — dla danego PGN opatrzyć każdy ruch oceną silnika i propozycją najlepszej kontynuacji; zaznaczyć błędy i przeoczone taktyki.
4. **Pobieranie zadań i partii historycznych z internetu** — np. partie Fischera, zadania kompozycyjne FIDE, zadania z Lichess. Jest to tryb **interaktywny**: program pyta użytkownika, które źródło/zawodnika/zestaw zadań pobrać, ściąga dane i ładuje je do analizy.
5. **Gra człowiek-człowiek i człowiek-komputer** — istnieje minimalny interfejs konsolowy, ale jest efektem ubocznym powyższych funkcji, nie głównym celem.

---

## 2. Grupa docelowa dla kodu źródłowego

Czytelnik kodu powinien być:

- Programistą znającym C++ na poziomie średniozaawansowanym, ale nowym w programowaniu szachowym, albo
- Programistą szachowym, który chce zobaczyć czystą, nowoczesną implementację klasycznych technik w C++.

Pisz dla tego czytelnika. Jeśli implementujesz alpha-beta, krótki blok komentarza na początku funkcji powinien przypomnieć czytelnikowi, czym jest alpha-beta i dlaczego działa to z tymi ograniczeniami. Jeśli implementujesz Zobrist hashing, wyjaśnij w kilku zdaniach, co jest hashowane i dlaczego kolizje są akceptowalne. Z nagłówków modułów linkuj do Chess Programming Wiki (`https://www.chessprogramming.org`), gdzie to zasadne.

---

## 3. Wymagania techniczne

- **Język:** preferowany C++23, minimum C++20. Używaj cech C++23 (`std::expected`, `std::print`, `std::format`, `std::mdspan` jeśli przydatne, `if consteval`, ulepszone ranges) tam, gdzie czynią kod czytelniejszym. **Nie** używaj cech C++23 tylko po to, by się popisać.
- **Build:** CMake ≥ 3.25. Presety (`CMakePresets.json`) dla `debug`, `release`, `release-with-asserts`.
- **Kompilatory:** musi budować się czysto na GCC ≥ 13 i Clang ≥ 17 pod Linuksem. Warningi jako błędy (`-Wall -Wextra -Wpedantic -Werror`).
- **Zależności:** tylko biblioteka standardowa dla rdzenia. `Catch2` przez `FetchContent` dla testów. Nic poza tym dla 0.x–1.0. `{fmt}` jest kuszący, ale niepotrzebny, jeśli dostępne są `std::print`/`std::format` z C++23. Bez Boosta.
- **Platformy:** Linux jest głównym celem. macOS powinien działać. Windows — best effort.
- **Wyjście:** jedna binarka konsolowa `chesserazade`. Oddzielna biblioteka statyczna `chesserazade_core` zawiera logikę silnika, dzięki czemu zarówno testy, jak i `chesserazade` linkują się do niej.

### Styl kodu (wiążący)

- **Szablony:** używaj tylko tam, gdzie wynikają naturalnie z DRY — np. mała funkcja numeryczna, która faktycznie ma sens dla wielu szerokości liczb całkowitych. **Nie** szablonizuj `Board`, `MoveGenerator`, `Search`. Polimorfizm, tam gdzie potrzebny, jest realizowany przez klasy abstrakcyjne i `virtual`, bo projekt jest dydaktyczny, a vtable to drobny, dobrze zrozumiały koszt.
- **Lambdy:** w zasadzie unikaj. Nazwana funkcja jest łatwiejsza do czytania i debugowania. `string_view`, `span`, `optional`, `expected`, `variant`, `array`, `bitset` są w porządku i zalecane tam, gdzie wyjaśniają intencję.
- **Bez makr** poza odpowiednikami include guardów (preferuj `#pragma once`). Bez `using namespace std;` na poziomie pliku.
- **Obsługa błędów:** `std::expected<T, Error>` dla błędów odwracalnych (nieudane parsowanie, zły FEN, nielegalny ruch z wejścia). `assert` dla niezmienników oznaczających błąd silnika. Brak wyjątków na gorących ścieżkach; wyjątki dopuszczalne tylko na granicy CLI/front-endu.
- **Nazewnictwo:** typy `PascalCase`, funkcje i zmienne `snake_case`, stałe `SCREAMING_SNAKE_CASE`, wartości enumów `PascalCase`. Nagłówki `.hpp`, pliki źródłowe `.cpp`. Jeden typ publiczny na nagłówek, gdy to sensowne.
- **Nagłówki:** każdy publiczny nagłówek zaczyna się blokiem `///` opisującym cel modułu, jego niezmienniki i link do odpowiedniej strony Chess Programming Wiki, jeśli istnieje.
- **Pliki ≤ ~400 linii.** Gdy plik rośnie powyżej tego, to sygnał, by podzielić go wg pojęć, nie wg liczby linii.

---

## 4. Przegląd architektury

```
+-----------------------------------------------------------+
|                        CLI / main                         |
|  dispatch komend, parsowanie argumentów, prompt interakt. |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                   Usługi aplikacyjne                      |
|  perft runner | solver zadań | analizator | net fetch     |
+-------+---------------+---------------+-------------------+
        |               |               |
+-------v------+ +------v------+ +------v---------+
|   Search     | | Evaluator   | |   PGN / FEN    |
|  minimax,    | | materiał,   | |   parsowanie   |
|  alpha-beta, | | piece-sq    | |   i serializ.  |
|  TT, ordering| | tables      | |                |
+-------+------+ +------+------+ +----------------+
        |               |
+-------v---------------v-----------------------------------+
|                    Generator ruchów                       |
|   generacja pseudo-legalnych, filtr legalności,           |
|   make/unmake, Zobrist hashing                            |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                       Board (interfejs)                   |
|     Board8x8Mailbox (1.0)   |   BoardBitboard (1.1)       |
+-----------------------------------------------------------+
```

### Kluczowe abstrakcje

- **`Board`** — interfejs abstrakcyjny: odczyt figury na polu, strona do ruchu, prawa do roszady, pole en passant, zegar połowicznych ruchów, numer pełnego ruchu, klucz Zobrista; `make_move` i `unmake_move`; równość po pozycji (nie po historii). Interfejs jest mały i stabilny, tak aby w 1.1 móc wprowadzić `BoardBitboard` bez zmian gdzie indziej.
- **`Move`** — zwięzły POD: pole źródłowe, pole docelowe, ruszana figura, bita figura, figura promocji, flagi (roszada, en passant, podwójny krok piona). 16 bitów jeśli upakowany, ale najpierw czytelność — struktura 32- lub 64-bitowa też jest OK.
- **`MoveGenerator`** — funkcje wolne lub bezstanowa klasa; bierze `const Board&`, zwraca `MoveList` (small-vector, stała pojemność 256 jest bezpieczna dla szachów). Udostępnia `generate_legal(board)` i `generate_pseudo_legal(board)`.
- **`Evaluator`** — bierze `const Board&`, zwraca ocenę w centypionkach z perspektywy strony do ruchu.
- **`Search`** — bierze `Board`, limit (głębokość, liczba węzłów lub czas), `Evaluator` i zwraca `SearchResult { best_move, score, principal_variation, nodes, elapsed }`.
- **`Pgn`, `Fen`** — czyste parsowanie/serializacja, bez stanu silnika.
- **`PuzzleSolver`** — orkiestruje `Search`, by znaleźć mata w N lub najlepszy ruch taktyczny; drukuje rozwiązania z czytelnym PV.
- **`GameAnalyzer`** — uruchamia `Search` w każdym półruchu partii PGN i raportuje delty oceny oraz proponowane alternatywy.
- **`NetFetcher`** — mała usługa (za interfejsem) pobierająca zadania/partie ze skonfigurowanego źródła. Interaktywna: pyta użytkownika o źródło i zapytanie.

---

## 5. Układ katalogów

```
chesserazade/
├── CMakeLists.txt
├── CMakePresets.json
├── HANDOFF.md                  (wersja angielska — kanoniczna)
├── README.md
├── LICENSE
├── docs/
│   ├── HANDOFF_pl.md           (ten plik)
│   ├── architecture.md         (pisany w 0.2, uaktualniany w każdej wersji)
│   ├── coding_style.md         (pisany w 0.1)
│   └── version_notes/
│       ├── 0.1.md
│       ├── 0.2.md
│       └── ...
├── include/chesserazade/       (nagłówki publiczne, używane przez testy i main)
│   ├── board.hpp
│   ├── move.hpp
│   ├── move_generator.hpp
│   ├── fen.hpp
│   ├── pgn.hpp                 (od 0.4)
│   ├── evaluator.hpp           (od 0.5)
│   ├── search.hpp              (od 0.5)
│   ├── zobrist.hpp             (od 0.7)
│   ├── transposition_table.hpp (od 0.7)
│   ├── puzzle_solver.hpp       (od 0.8)
│   ├── game_analyzer.hpp       (od 0.9)
│   └── net_fetcher.hpp         (od 1.0)
├── src/
│   ├── main.cpp
│   ├── cli/
│   │   ├── command_dispatch.cpp
│   │   ├── cmd_perft.cpp
│   │   ├── cmd_solve.cpp
│   │   ├── cmd_analyze.cpp
│   │   └── ...
│   ├── board/
│   │   ├── board8x8_mailbox.cpp
│   │   └── board8x8_mailbox.hpp
│   ├── move_generator/
│   ├── search/
│   ├── eval/
│   ├── io/
│   └── net/
├── tests/
│   ├── CMakeLists.txt
│   ├── test_fen.cpp
│   ├── test_move_generator.cpp
│   ├── test_perft.cpp
│   ├── test_search.cpp
│   ├── test_pgn.cpp
│   └── data/
│       ├── perft_positions.txt
│       ├── puzzles_mate_in_2.txt
│       └── sample_games.pgn
└── tools/
    └── perft_bench.cpp         (opcjonalna samodzielna binarka perft)
```

---

## 6. Powierzchnia CLI

Binarka `chesserazade` wywoływana jest z podkomendą. Każda podkomenda drukuje `--help`. Przykłady:

```
chesserazade perft --depth 5 --fen <fen>
chesserazade perft --depth 5 --divide                 # rozpisuje liczniki per-ruch
chesserazade moves --fen <fen>                        # listuje legalne ruchy w pozycji
chesserazade show --fen <fen>                         # ładnie drukuje planszę
chesserazade solve --fen <fen> --mate-in 2
chesserazade solve --fen <fen> --depth 8
chesserazade analyze --pgn game.pgn --depth 12
chesserazade fetch                                    # prompt interaktywny
chesserazade play                                     # minimalna gra interaktywna (od 0.4)
chesserazade repl                                     # interaktywna powłoka ze wszystkimi komendami (od 0.4)
chesserazade version
```

Zasady:

- Każda podkomenda jest zaimplementowana w osobnym pliku pod `src/cli/`.
- Format wejściowy pozycji to FEN. Dla ruchów akceptuj UCI (`e2e4`, `e7e8q`) i opcjonalnie SAN (`Nf3`) od 0.4.
- Każda podkomenda, która może trwać, drukuje końcową linię podsumowania: `nodes N, time T ms, N/s`.

---

## 7. Strategia testowania

- **Catch2** ściągany przez `FetchContent`.
- **Poprawność perft jest bezwzględnym wymaganiem.** Generator ruchów musi przechodzić sześć standardowych pozycji perft (patrz `https://www.chessprogramming.org/Perft_Results`) do głębokości ≥ 5 (pozycja startowa do głębokości 6). Od wersji 0.2 staje się to testem CI.
- **Testy round-trip FEN:** parsowanie → serializacja → parsowanie musi być stabilne.
- **Testy round-trip PGN** od 0.4.
- **Testy regresyjne szukania:** garść znanych zadań mat-w-2, mat-w-3 i taktyk ze znanymi odpowiedziami, od 0.5.
- **Żadnych flaky testów.** Testy z limitem czasowym są w CI zabronione; wyłącznie limit głębokości lub liczby węzłów.
- **Benchmarki to nie testy.** `tools/perft_bench.cpp` może istnieć, ale nie jest uruchamiany w CI.

---

## 8. Pobieranie z internetu (1.0)

Podkomenda `fetch` jest interaktywna. Oczekiwany przepływ:

1. Program drukuje menu dostępnych źródeł (początkowo: Lichess puzzle API, repozytorium partii szachowych takie jak `pgnmentor.com` lub jego lokalne lustro, URL podany przez użytkownika).
2. Użytkownik wybiera źródło.
3. Program pyta o zapytanie (nazwisko zawodnika, motyw, przedział ratingu zadań, przedział dat).
4. Program pobiera dane, cachuje je pod `~/.cache/chesserazade/` i proponuje załadowanie do analizy.

Ograniczenia projektowe:

- **Odizoluj kod sieciowy za interfejsem `NetFetcher`**, tak by reszta programu (ani testy) nigdy nie dotykała sieci bezpośrednio. Testy wstrzykują atrapę fetchera.
- **Respektuj limity i warunki użytkowania** każdego zewnętrznego API.
- **Wszystkie wywołania sieciowe są jawne, logowane i wymagają potwierdzenia przez użytkownika** przed wysłaniem. Żadnego ruchu w tle.
- **Agresywne cachowanie.** Zestaw zadań lub archiwum partii pobrane raz nie powinno być pobierane ponownie.

Od agenta implementującego oczekuje się, że potwierdzi z użytkownikiem, które konkretne źródła wspierać w 1.0; Lichess puzzles + jedno źródło partii historycznych to rozsądne minimum.

---

## 9. Mapa wersji

Każda wersja kończy się podpisanym tagiem gita (`v0.1`, `v0.2`, …). Każda wersja produkuje `docs/version_notes/<version>.md` podsumowujący, co dodano, jakie testy przechodzą i co wiadomo, że brakuje.

### 0.1 — Fundamenty
- Interfejs `Board` + implementacja `Board8x8Mailbox` (bez en passant, bez roszad — jedynie rozstawienie figur, strona do ruchu).
- Parsowanie i serializacja FEN.
- Komenda `show --fen`.
- Typ `Move` i drukarka ruchów (notacja UCI).
- Szkielet projektu: CMake, presety, `.clang-format`, stub CI, podpięty Catch2.
- **Acceptance:** `chesserazade show --fen "<standardowy FEN pozycji startowej>"` renderuje planszę. Testy round-trip FEN przechodzą.

### 0.2 — Pełna generacja legalnych ruchów
- Generacja pseudo-legalna dla wszystkich figur.
- Filtr legalności (król nie w szachu po ruchu).
- **Wszystkie ruchy specjalne:** roszady (obie strony, oba kolory, poprawne prawa i zasada przechodzenia przez szach), en passant, promocje (wszystkie cztery figury).
- `make_move` / `unmake_move` z pełnym przywracaniem stanu.
- Komenda `moves --fen <fen>`.
- Komenda `perft --depth N --fen <fen>` z opcją `--divide` i pomiarem czasu.
- **Acceptance:** przechodzi sześć standardowych pozycji perft do głębokości 5 (pozycja startowa do głębokości 6). CI wymusza.

### 0.3 — Utrwalenie generatora
- Zestaw testów perft podpięty do CI.
- Mikrobenchmarki w `tools/perft_bench.cpp` do użytku implementującego (nie uruchamiane w CI).
- Runda dokumentacyjna: każdy publiczny nagłówek ma blok dokumentacyjny; szkic `docs/architecture.md`.
- **Acceptance:** perft na standardowych pozycjach do głębokości 6 kończy się w rozsądnym czasie (jednocyfrowa liczba minut jest akceptowalna dla implementacji mailbox — szybkość to nie jest jeszcze cel).

### 0.4 — PGN, historia, minimalna gra interaktywna
- Parser i pisak PGN (pełne pary tagów, ruchy SAN, komentarze, warianty opcjonalnie — w minimum SAN głównej linii).
- Stos historii ruchów w klasie `Game` nad `Board`.
- Podkomenda `repl` / `play`: mały tekstowy UI do wprowadzania ruchów (UCI i SAN), cofanie, wyświetlanie, zapis do PGN.
- **Acceptance:** można wczytać przykładowy PGN, przejść go krok po kroku, zapisać z powrotem, a diff jest semantycznie równoważny.

### 0.5 — Ocena materialna + minimax
- `Evaluator` z wartościami figur i tablicami pole-figura (klasyczne wartości z Chess Programming Wiki).
- `Search` ze zwykłym minimaxem (negamax), stała głębokość.
- Podkomenda `solve --fen <fen> --depth N`.
- **Acceptance:** znajduje wymuszonego mata-w-1 i mata-w-2 na standardowych pozycjach testowych.

### 0.6 — Alpha-beta + iterative deepening + kontrola czasu
- Alpha-beta pruning zastępuje zwykły minimax.
- Iterative deepening.
- Limit czasu (`--time-ms`) i limit węzłów (`--nodes`).
- Wyjście z wariantem głównym (PV).
- **Acceptance:** rozwiązuje mata-w-3 na standardowych pozycjach testowych; PV jest wyświetlane; szukanie respektuje limit czasu z dokładnością ~5%.

### 0.7 — Zobrist hashing + tablica transpozycji
- Klucze Zobrista utrzymywane przyrostowo w `make_move` / `unmake_move`.
- Tablica transpozycji stałego rozmiaru z schematem wymiany (wiek + głębokość).
- Odczyty i zapisy TT w `Search`.
- To adresuje uwagę użytkownika o „cechowaniu ruchów A,B,C = C,B,A" — TT oparte na kluczu Zobrista wykrywa transpozycje niezależne od kolejności ruchów.
- **Acceptance:** mierzalna redukcja liczby węzłów na pozycjach taktycznych; logowany hit rate TT.

### 0.8 — Uporządkowanie ruchów + quiescence + solver zadań
- Uporządkowanie ruchów: ruch z TT, MVV-LVA dla bić, killer moves, history heuristic.
- Quiescence search dla bić (i opcjonalnie szachów).
- `PuzzleSolver` z opcją `--mate-in N`: używa negamax z mate-scoringiem.
- **Acceptance:** rozwiązuje wyselekcjonowany zestaw zadań mat-w-2 i mat-w-3 z poprawnym pierwszym ruchem i pełnym PV.

### 0.9 — Analizator partii
- `analyze --pgn file.pgn --depth N`: dla każdej pozycji raportuje ocenę silnika, najlepszy ruch i oznacza błędy (skok oceny > próg).
- Format wyjścia: oznaczony PGN z glifami NAG (`?`, `??`, `!`, `!!`, `?!`, `!?`) i komentarzami PV.
- **Acceptance:** produkuje sensowny oznaczony PGN dla znanej „błędnej" partii przykładowej.

### 1.0 — Pobieranie z internetu, polerowanie, dokumentacja
- Podkomenda `fetch` zgodnie z §8.
- Pełny przegląd dokumentacji: `docs/architecture.md` odzwierciedla stan końcowy; każdy moduł ma blok dokumentacyjny.
- `README.md` przepisany, z instrukcjami budowania, listą cech i szybkim startem.
- Sekcja „znane ograniczenia" w `README.md` i w każdym module.
- Tag wersji `v1.0`.
- **Acceptance:** użytkownik może pobrać partie Fischera przez `fetch`, uruchomić `analyze` na jednej z nich i przeczytać wynik.

### 1.1 — Alternatywny bitboard
- Implementacja `BoardBitboard` interfejsu `Board`.
- Magic bitboards (lub PEXT, gdzie dostępne) dla figur sunących. To jedyne miejsce, w którym oczekuje się nieco złożonych stałych i kodu generacji tablic.
- Flaga runtime'owa `--board=mailbox|bitboard` (lub opcja CMake) wybiera, której implementacji używa binarka. Obie przechodzą te same testy perft. Implementacja mailbox **pozostaje** w drzewie jako referencja.
- **Acceptance:** `perft` z bitboardem przechodzi ten sam zestaw poprawności i jest mierzalnie szybszy od mailboxa.

### 1.2 — GUI Qt6
- Front-end Qt6 jako osobny cel CMake `chesserazade_gui`, linkujący `chesserazade_core`.
- Wyświetlanie planszy, ruchy drag-and-drop, load/save PGN, wywoływanie szukania i rozwiązywacza zadań, pasek oceny i PV.
- **Acceptance:** istniejąca binarka konsolowa pozostaje nienaruszona; GUI jest wyłącznie dodatkowym front-endem.

### TODO po 1.2 (bez planowania szczegółowego)
- **Protokół UCI** — aby silnik dawał się wpiąć w Arenę, CuteChess, SCID itd. Specyfikacja: `https://wbec-ridderkerk.nl/html/UCIProtocol.html`.
- **Księga debiutów** (format Polyglot) jako opcjonalne źródło tylko do odczytu.
- **Odczyt tablic końcówkowych** (Syzygy) jako opcjonalne źródło tylko do odczytu.
- **Szukanie wielowątkowe (SMP)**, np. Lazy SMP.
- **NNUE** jest jawnie poza zakresem — podważałoby „klasyczną, edukacyjną" tożsamość projektu.

---

## 10. Przepływ pracy i deliverables między wersjami

- **Jedna gałąź cech na wersję.** Merge przez squash albo czysty merge commit do `main`. Zatag wynik.
- **Każdy PR wersji musi:**
  - listować, co dodano,
  - linkować do odpowiedniego `docs/version_notes/<version>.md`,
  - pokazywać, że wszystkie wcześniejsze testy nadal przechodzą,
  - pokazywać nowe testy wprowadzone dla tej wersji.
- **Wiadomości commitów:** tryb rozkazujący, scoped (`move-gen: add en-passant generation`), treści wyjaśniają *dlaczego*, gdy to nie jest oczywiste.
- **Bez zakomentowanego kodu** w `main`. Usuń go; git pamięta.
- **Bez TODO/FIXME** w `main` bez powiązanego ticketa lub notki w `docs/version_notes/`.

---

## 11. Otwarte pytania dla agenta implementującego

Rozwiąż to z użytkownikiem przed rozpoczęciem odpowiedniej wersji:

1. **Zakres PGN w 0.4:** warianty i NAG-i w 0.4 czy odroczone do 0.9? (Rekomendacja: parsować i zachowywać, ignorować w analizie 0.4; używać w 0.9.)
2. **Format wyjścia analizy w 0.9:** tylko oznaczony PGN, czy też plain-text raport? (Rekomendacja: oba, plain-text jako domyślny, `--pgn-out` by zapisać oznaczony PGN.)
3. **Źródła fetch w 1.0:** które konkretne API/serwisy? Lichess puzzle API jest oczywistym startem; potwierdź, które źródło partii historycznych (pgnmentor.com, archiwum chessgames.com, eksport Lichess study czy lokalny katalog PGN) preferuje użytkownik.
4. **Rygor parsera SAN:** odrzucać niejednoznaczne SAN? (Rekomendacja: tak, z czytelnym komunikatem błędu.)
5. **Kodowanie wyjścia konsoli:** używać figurynek Unicode (`♔♕♖♗♘♙`) domyślnie? Fallback do ASCII (`KQRBNP`) przy `--ascii`? (Rekomendacja: tak, Unicode domyślnie, fallback ASCII.)

---

## 12. Tłumaczenie

Wersja kanoniczna to `HANDOFF.md` w katalogu głównym (po angielsku). Ten plik (`docs/HANDOFF_pl.md`) jest jej wierną polską wersją. Utrzymujmy oba pliki w synchronizacji przy każdej kolejnej edycji.

---

*Koniec handoffu. W razie wątpliwości — optymalizuj dla czytelnika kodu źródłowego, nie dla benchmarka.*

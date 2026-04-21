# Chesserazade — Architektura

Szkic: 0.5 (odzwierciedla kod do tagu `v0.5.0`). Aktualizowane
przy każdej wersji.

Ten dokument jest dla czytelnika, który sklonował drzewo i chce
mieć mapę przed zanurzeniem się w źródła. Uzupełnia
`HANDOFF.md` (plan wdrożenia), opisując to, co **jest dziś w
kodzie**, jak to się składa w całość i gdzie wejdą podsystemy 0.4+.

---

## 1. Kształt programu

Na najwyższym poziomie mamy **jedną bibliotekę statyczną** i
**dwa pliki wykonywalne**:

- `chesserazade_core` — silnik. Cała logika szachowa (typy,
  plansza, FEN, generator ruchów) jest tutaj. Żadnego I/O poza
  zwracanymi stringami.
- `chesserazade` — CLI. Linkuje `chesserazade_core`; cienki
  dispatcher po podkomendach.
- `perft_bench` — narzędzie deweloperskie (z `tools/`), również
  linkujące `chesserazade_core`. Nie jest częścią testów.

Binarka testów (`chesserazade_tests`) linkuje tę samą bibliotekę.
Publiczne API biblioteki (w `include/chesserazade/`) to to, z
czego korzystają zarówno testy, jak i CLI — nie ma „wewnętrznej"
furtki w obejściu tego API w kodzie produkcyjnym. Testy mają
prawo zaglądać do `src/`, by testować jednostkowo konkretny
`Board8x8Mailbox`, a nie tylko abstrakcyjny `Board`.

---

## 2. Warstwowa architektura — stan dzisiejszy

```
+-----------------------------------------------------------+
|                       CLI / main                          |
|   src/main.cpp + src/cli/ — rozsyłanie argumentów, jeden  |
|   cmd_*.cpp na podkomendę: show, moves, perft, repl/play, |
|   solve, version                                          |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|             Search + Evaluator   (nowe w 0.5)             |
|   Search::find_best — negamax na stałej głębokości        |
|   ze scoringiem matów + triangularna tablica PV           |
|   evaluate(board) — materiał + tablice piece-square       |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|              Game + PGN / SAN   (nowe w 0.4)              |
|   Game: Board + pozycja startowa + vector<Move> historia  |
|   SAN: parse/write Standard Algebraic Notation            |
|   PGN: parse/write Portable Game Notation                 |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                    Generator ruchów                       |
|   generate_pseudo_legal, generate_legal,                  |
|   is_in_check, is_square_attacked                         |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                    Board (interfejs)                      |
|   Board8x8Mailbox (0.1+)  |   BoardBitboard (plan 1.1)    |
+-------------------------+---------------------------------+
                          |
+-------------------------v---------------------------------+
|                   Typy rdzenia + I/O FEN                  |
|   Piece, Color, Square, File, Rank, Move, MoveKind,       |
|   CastlingRights — słownictwo, z którego korzystają wszyscy|
+-----------------------------------------------------------+
```

Warstwy, których **jeszcze nie ma** (planowane wg HANDOFF §9):

- **Alfa-beta + iteracyjne pogłębianie + kontrola czasu**
  (0.6) — wpina się w to samo miejsce wywołania co negamax.
- **Zobrist + TT** (0.7).
- **Sortowanie ruchów + quiescence + puzzle solver** (0.8).
- **Analizator gier** (0.9), **net fetcher** (1.0).

Każda z nich wchodzi *pomiędzy* generator ruchów a CLI, bez
potrzeby zmian poniżej.

---

## 3. Przepływ danych dla dwóch istniejących komend

### `chesserazade show --fen <fen>`

```
   FEN jako tekst ──► Board8x8Mailbox::from_fen ──► Board8x8Mailbox
                                                         │
                                                         ▼
                                               cli::cmd_show renderuje
                                               (ASCII lub Unicode)
```

Tylko `from_fen` dotyka konkretnego typu mailbox w CLI.
Rendering chodzi po abstrakcyjnym interfejsie `Board`, więc gdy
w 1.1 pojawi się `BoardBitboard`, nic w `cmd_show` nie trzeba
zmieniać.

### `chesserazade perft --depth N --fen <fen>`

```
   FEN ──► Board8x8Mailbox
                │
                ▼
           rekurencyjny perft
                │
                ▼
   MoveGenerator::generate_legal ──► MoveList
                │
                ▼
   dla m z listy: board.make_move(m); perft(depth-1); board.unmake_move(m)
```

Rekursja trzyma jedną mutowalną planszę i chodzi po niej przez
`make_move` / `unmake_move` — **żadnych kopii na węzeł**. Stosik
snapshotów wewnątrz `Board8x8Mailbox` to dokładnie to, co czyni
to bezpiecznym.

---

## 4. Kluczowe abstrakcje

### `Board` (`include/chesserazade/board.hpp`)

Klasa abstrakcyjna. Siedem zapytań tylko do odczytu (`piece_at`,
`side_to_move`, `castling_rights`, `en_passant_square`,
`halfmove_clock`, `fullmove_number` + `CastlingRights::any`)
oraz dwa mutatory (`make_move`, `unmake_move`). Mutatory
odkładają snapshot prywatny dla implementacji, by `unmake_move`
potrafiło dokładnie odtworzyć pozycję.

**Dlaczego abstrakcja:** 1.1 doda implementację bitboard tego
samego interfejsu. Wszystko powyżej tej linii — generator,
testy, CLI — jest napisane przeciw `Board&`, więc podmiana jest
lokalna.

### `Board8x8Mailbox` (`src/board/board8x8_mailbox.hpp`)

Konkretna implementacja z 0.x: `array<Piece, 64>` indeksowana
wg LERF (A1=0 … H8=63) plus pola skalarne na stronę / prawa
roszady / EP / zegary. Wektor snapshotów historii przechowuje
te trzy pola, których `Move` nie jest w stanie odtworzyć sam
(poprzednie pole EP, prawa roszady, halfmove clock).

### `Move` + `MoveKind` (`include/chesserazade/move.hpp`)

`Move` to `{ from, to, promotion, kind, moved_piece,
captured_piece }`. `MoveKind` to 8-wariantowy enum dobrany tak,
by był wyczerpujący i rozłączny: `Quiet`, `DoublePush`,
`KingsideCastle`, `QueensideCastle`, `Capture`, `EnPassant`,
`Promotion`, `PromotionCapture`. `make_move` switchuje po
`kind`, by zrobić odpowiedni efekt uboczny; każda inna warstwa
(wypisywanie UCI, ewaluacja, search) może `kind` ignorować.

### `MoveGenerator` (`include/chesserazade/move_generator.hpp`)

Bezstanowy (`MoveGenerator() = delete`). Metody statyczne:

- `generate_pseudo_legal(const Board&)` — wszystkie ruchy, które
  dopuszczają typy bierek, bez względu na to, czy własny król
  zostanie w szachu.
- `generate_legal(Board&)` — pseudo-legalne plus filtr
  `make_move`/`is_in_check`/`unmake_move`. To jedyne wejście,
  którego zwykle potrzebują wywołujący.
- `is_in_check(const Board&, Color)` — skanuje planszę w
  poszukiwaniu króla i pyta, czy jego pole jest atakowane.
- `is_square_attacked(const Board&, Square, attacker)` —
  klasyczna technika „patrz z celu na zewnątrz": rzuca promienie
  i skoki na zewnątrz od `sq`; jeśli pierwsza bierka na
  promieniu lub na przesunięciu skoczka/pionka to odpowiedni
  typ wrogi, zwraca true.

### `MoveList` (ten sam nagłówek)

`array<Move, 256>` o stałej pojemności z licznikiem. 256 to
wygodny zapas ponad teoretyczne maksimum 218 legalnych ruchów
w dowolnej osiągalnej pozycji. Uzasadnienie: search alokuje
jedną `MoveList` na węzeł; alokacja stosowa jest przyjazna
cache'owi i trywialnie recyklowana.

---

## 5. Układ katalogów (dziś)

```
chesserazade/
├── CMakeLists.txt              biblioteka + chesserazade + perft_bench
├── CMakePresets.json           debug / release / release-with-asserts
├── HANDOFF.md                  plan wdrożenia (+ docs/HANDOFF_pl.md)
├── README.md
├── LICENSE
│
├── include/chesserazade/       publiczne nagłówki — API biblioteki
│   ├── board.hpp               abstrakcyjny Board + CastlingRights
│   ├── fen.hpp                 serialize_fen + STARTING_POSITION_FEN
│   ├── move.hpp                Move, MoveKind, to_uci
│   ├── move_generator.hpp      MoveList + MoveGenerator
│   └── types.hpp               Color, Piece, Square, File, Rank
│
├── src/
│   ├── main.cpp
│   ├── cli/                    po jednym cmd_*.cpp na podkomendę
│   │   ├── command_dispatch.{hpp,cpp}
│   │   ├── cmd_show.{hpp,cpp}
│   │   ├── cmd_moves.{hpp,cpp}
│   │   ├── cmd_perft.{hpp,cpp}
│   │   └── cmd_version.{hpp,cpp}
│   ├── board/
│   │   └── board8x8_mailbox.{hpp,cpp}
│   ├── move_generator/
│   │   └── move_generator.cpp
│   └── io/
│       ├── fen.cpp
│       └── move.cpp
│
├── tests/                      Catch2 v3 przez FetchContent
│   ├── CMakeLists.txt
│   ├── test_types.cpp
│   ├── test_move.cpp
│   ├── test_board.cpp
│   ├── test_fen.cpp
│   └── test_perft.cpp          sześć standardowych pozycji, głęb. 1..5
│
├── tools/
│   └── perft_bench.cpp         pomiar szybkości, nie uruchamiany w testach
│
└── docs/
    ├── HANDOFF_pl.md
    ├── architecture.md         (ten plik) + architecture_pl.md
    ├── coding_style.md         + coding_style_pl.md
    └── version_notes/
        ├── 0.1.md + 0.1_pl.md
        ├── 0.2.md + 0.2_pl.md
        └── 0.3.md + 0.3_pl.md
```

Dokumentacja dwujęzyczna: wersja angielska jest kanoniczna;
`*_pl.md` to polskie tłumaczenie utrzymywane w synchronizacji.

---

## 6. Niezmienniki projektowe

To są kontrakty, na których stoi kod. Złamanie ich to bug; kilka
z nich sprawdzają testy.

1. **Mapowanie LERF pól.** Indeksy `Square` są rank-major,
   file-minor. `make_square(file, rank)`, `file_of`, `rank_of`
   i literały `A1=0 … H8=63` w `types.hpp` to jedyne konwersje;
   żadna warstwa nie rozwija własnej.
2. **FEN jest ASCII.** Parsowanie odrzuca nie-ASCII.
   Serializacja emituje tylko ASCII. Unikodowe figurki to wybór
   renderingu, nie format danych — flaga `--unicode` włącza je
   tylko do wyświetlenia.
3. **Legalność ruchu to sprawa generatora.** `Board::make_move`
   zakłada, że wywołujący już odfiltrował ruch pod kątem
   legalności. `make_move` sprawdza asercjami podstawowe
   niezmienniki (poprawność pól), ale nie weryfikuje ponownie,
   że ruch jest legalny w sensie reguł szachowych.
4. **`unmake_move` jest dokładny.** Po `make_move(m);
   unmake_move(m)` plansza jest polem-w-pole równa tej sprzed
   wywołania (równość pomija efemeryczny stosik historii).
5. **Prawa roszady maleją monotonicznie.** Mogą zniknąć przez
   ruch króla lub wieży albo przez bicie na polu narożnym wieży.
   W obrębie gry nigdy nie są przywracane.
6. **Bez wyjątków na gorącej ścieżce.** Sygnalizacja błędów w
   silniku używa `std::expected` (FEN) lub asercji (pogwałcenie
   niezmiennika). Wyjątki są dozwolone tylko na granicy CLI.
7. **Pliki poniżej ~400 linii.** Przekroczenie to sygnał do
   podziału wg pojęć. (To nie twarda reguła, ale silny
   domyślny kierunek.)

---

## 7. Gdzie wpinają się nowe podsystemy

Kiedy zjedzie kolejna wersja, to są punkty rozszerzenia:

- **0.4 PGN / Game / repl** — `Game` siedzi *ponad* `Board`:
  trzyma `Board` plus wektor `Move` dla undo/redo i wyświetlania
  SAN. `Pgn` to czyste I/O tekstowe, niezależne od konkretnego
  typu `Board`.
- **0.5 Evaluator** — nowy publiczny nagłówek
  `include/chesserazade/evaluator.hpp`, klasa konkretna w
  `src/eval/`. Bierze tylko `const Board&`; nie mutuje.
- **0.5 Search** — nowy publiczny nagłówek `search.hpp`,
  `src/search/`. Bierze mutowalny `Board&`, `Evaluator&`,
  limit oraz `MoveGenerator` niejawnie (przez wywołania
  statyczne). Zwraca `SearchResult`.
- **0.7 Zobrist + TT** — klucz hasha jest utrzymywany
  inkrementalnie wewnątrz `make_move` / `unmake_move` w
  `Board`. TT to nowy podsystem w `src/search/`, z którym
  konsultuje się `Search`.
- **1.1 BoardBitboard** — druga implementacja interfejsu
  `Board`. Mailbox zostaje w drzewie jako implementacja
  referencyjna; testy działają na obu przez typ abstrakcyjny.

Wspólny motyw: interfejs `Board` jest stabilnym kontraktem;
każdy nowy podsystem albo zależy od tego interfejsu, albo
siedzi ponad nim.

---

## 8. Rzeczy, których świadomie *nie* zrobiliśmy

- **Brak szablonów na `Board` / `MoveGenerator` / `Search`.**
  Koszt polimorfizmu jest pomijalny, a vtable jest bardziej
  czytelną abstrakcją dla tej grupy odbiorców.
- **Brak „Position" / „State" / „Game" god-class'y w rdzeniu.**
  Każdy problem to osobny typ; `Game` (0.4) to cienka
  kompozycja `Board` + historia ruchów, nie przepisanie na
  nowo.
- **Brak lambd w gorącym kodzie.** Nazwane funkcje łatwiej
  debugować krok po kroku i łatwiej wyszukiwać gre'em.
- **Brak zewnętrznych zależności poza Catch2.** Biblioteka
  standardowa pokrywa wszystko, czego potrzebujemy w 0.x.
- **Brak NNUE, brak sieci neuronowych.** Tylko klasyczne
  techniki — to tożsamość projektu.

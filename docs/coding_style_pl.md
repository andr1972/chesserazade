# Styl kodu

Dokument ten ustala wiążące konwencje pisania kodu C++ w
`chesserazade`. Wszystko poniżej to powtórzenie
[HANDOFF.md §3](../HANDOFF.md) plus kilka decyzji doprecyzowanych
w trakcie rozwoju kodu.

## Język i narzędzia

- **C++23** to podstawa. C++20 jest dopuszczalnym minimum
  wyłącznie w razie koniecznego zejścia w przyszłym refaktorze;
  obecnie przyjmujemy 23.
- Kompilatory wspierane w CI: **GCC ≥ 13**, **Clang ≥ 17**.
  Referencyjna maszyna buildu używa GCC 15 / Clang 20.
- **CMake ≥ 3.25**, sterowany przez `CMakePresets.json`
  (`debug`, `release`, `release-with-asserts`).
- Ostrzeżenia są błędami. Wspólny interfejs `chesserazade_warnings`
  niesie: `-Wall -Wextra -Wpedantic -Werror -Wshadow
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Woverloaded-virtual -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion -Wformat=2`.

## Nazewnictwo

| Kategoria  | Styl              | Przykład                    |
|------------|-------------------|-----------------------------|
| Typ        | `PascalCase`      | `Board8x8Mailbox`           |
| Funkcja    | `snake_case`      | `serialize_fen`, `piece_at` |
| Zmienna    | `snake_case`      | `side_to_move`, `ep_square` |
| Stała      | `SCREAMING_SNAKE` | `NUM_SQUARES`, `STARTING_POSITION_FEN` |
| Wariant enuma | `PascalCase`   | `enum class Color { White, Black };` |
| Plik       | `snake_case.hpp`/`.cpp` | `board8x8_mailbox.hpp` |

## Układ plików

- Publiczne nagłówki leżą w `include/chesserazade/`.
- Prywatne nagłówki leżą obok swojej implementacji w `src/`.
- **Jeden typ publiczny na nagłówek** tam, gdzie to praktyczne.
- Każdy publiczny nagłówek zaczyna się blokiem `///` opisującym
  cel modułu, jego niezmienniki i link do strony Chess
  Programming Wiki, jeśli istnieje.
- Pliki źródłowe celują w **≤ 400 linii**. Kiedy plik przekracza,
  dzieli się go po koncepcie, nie po liczbie linii.

## Komentarze

Ten projekt świadomie **łamie domyślne "bez komentarzy"**. Czytelnik
przychodzący do modułu po raz pierwszy powinien zorientować się z
bloku dokumentacji nagłówka i komentarzy nad nietrywialnymi
funkcjami. Konkretnie:

- **Tak, komentuj.** Dodawaj blok dokumentacji do każdej publicznej
  funkcji, typu i nagłówka. Opisuj niezmienniki, konwencje
  współrzędnych i — przy implementacji klasycznego algorytmu — czym
  jest i dlaczego działa (np. co przycina alpha-beta, dlaczego
  kolizje Zobrista są akceptowalne).
- **Nie komentuj oczywistości.** `// inkrementuj i` to szum. Nazwij
  zmienną pętli dobrze i zostaw to.
- **Odnosząc się do Chess Programming Wiki** w nagłówkach
  implementujących klasyczne techniki.

## Cechy języka

- **Szablony:** tylko tam, gdzie wynikają naturalnie z DRY. *Nie*
  templatyzuj `Board`, `MoveGenerator`, `Search`. Polimorfizm
  używa abstrakcyjnych klas bazowych i `virtual`.
- **Wyrażenia lambda:** unikane. Nazwana lokalna funkcja lub wolny
  helper jest łatwiejsza do debugowania krokowego.
- **Makra:** brak, poza strażnikami include w stylu `#pragma once`.
- **`using namespace std;`:** nigdy w zasięgu pliku.
- **Obsługa błędów:**
  - `std::expected<T, Error>` dla błędów odzyskiwalnych
    (parsowanie, niepoprawny FEN, nielegalny ruch od użytkownika).
    Wzór: `FenError` w `chesserazade/fen.hpp`.
  - `assert` dla niezmienników sygnalizujących błąd w silniku.
  - Wyjątki tylko na granicy CLI / warstwy prezentacji.

## Formatowanie

Sterowane przez `.clang-format` w korzeniu repo. Najważniejsze:

- Wcięcie 4 spacje, bez tabulatorów, 100-kolumnowy limit miękki.
- Klamry w tej samej linii co instrukcja sterująca.
- Gwiazdka przy typie: `int* p`, nie `int *p`.
- Nagłówki grupowane: najpierw `<chesserazade/...>`, potem
  cudzysłowowe include-y projektowe, potem systemowe `<...>`.

Jeśli musisz odejść od `.clang-format`, napisz nad blokiem jedno
zdanie wyjaśniające *dlaczego*.

## Testy

- Catch2 v3 pobierane przez `FetchContent`. Żadnych innych
  zależności testowych.
- Testy leżą w `tests/`, jeden `test_<moduł>.cpp` na moduł.
- **Deterministyczne.** Żadnych testów czasowych w CI; gdy w grze
  jest wyszukiwanie — używaj limitu głębokości lub węzłów.
- Testy mogą sięgać do `src/` po prywatne nagłówki: ścieżka
  `src/` jest dodana do include-ów binarki testowej.

## Komunikaty commitów

- Tryb rozkazujący, z zakresem (`fen: accept non-canonical castling
  order`).
- Ciało wyjaśnia *dlaczego*, gdy nieoczywiste. Czasem
  teraźniejszym, nie "zrobię X".
- **Bez `--no-verify`**, bez pomijania hooków. Jeśli hook pada —
  napraw przyczynę źródłową.

## Dokumentacja dwujęzyczna

Dokumenty projektowe mają polskiego bliźniaka. Kanoniczna wersja
angielska leży w swojej właściwej lokalizacji
(np. `HANDOFF.md`, `docs/coding_style.md`); polskie tłumaczenie
leży obok z sufiksem `_pl`
(`docs/HANDOFF_pl.md`, `docs/coding_style_pl.md`). Oba są
synchronizowane przy każdej edycji.

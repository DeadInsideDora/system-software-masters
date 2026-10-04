# Лабораторная работа 3 — вариант 3

**Полная сборка мусора методом копирования выживших.**

Сборщик использует два полупространства и алгоритм Чейни: переносит достижимые
объекты, обновляет корни и ссылки, сохраняет циклы и общие ссылки.
При нехватке места сборка запускается автоматически.

Сборщик написан на учебном языке в `lab3/runtime/gc.src`. Компилятор на C
преобразует его вместе с программой в байткод MoarVM. Управляемая куча
размещена внутри массива; достижимость объектов в ней определяет
реализованный сборщик.

## Требования

- GCC или Clang, Make, Flex, Bison;
- MoarVM 2026.08;
- Python 3 для скриптов демонстрации и построения графиков;
- Matplotlib для графиков (устанавливается командой `make setup-plots`);
- для установки MoarVM скриптом: Perl, curl, tar, shasum и доступ к интернету.

Команды выполняются из корня проекта в macOS или Linux.

## Сборка и запуск

```sh
git clone --branch lab3 git@github.com:DeadInsideDora/system-software-masters.git
cd system-software-masters
make setup-vm
make
make demo
```

`make setup-vm` устанавливает MoarVM в `lab3/.local/moarvm`.
Если VM уже установлена, этот шаг можно пропустить и указать её путь:

```sh
export MOAR=/absolute/path/to/moar
```

| Команда | Демонстрация |
| --- | --- |
| `make demo` | Перенос живого графа, циклы, общие ссылки и освобождение мусора |
| `make demo-animals` | Сохранение кошек после удаления собак, автоматические сборки и снятие корней |
| `make demo-blackbox` | Случайные выделения, изменения ссылок и снятие корней |

Для основного демо можно задать размеры памяти и число временных выделений:

```sh
make demo LAB3_HEAP_WORDS=4096 LAB3_SPACE_WORDS=32 LAB3_ROOTS=16 LAB3_ROUNDS=200
```

Размеры задаются в 64-битных словах. `LAB3_HEAP_WORDS` — размер общего массива,
`LAB3_SPACE_WORDS` — одного полупространства, `LAB3_ROOTS` — ёмкость корней,
`LAB3_ROUNDS` — число временных выделений.

Прямая компиляция и запуск без Python:

```sh
mkdir -p lab3/output
lab3/build/lab3c --heap-words 4096 --arg 128 --arg 16 --arg 200 \
  lab3/output/copying.moarvm lab3/runtime/gc.src lab3/examples/copying.src
./lab3/tools/moar.sh lab3/output/copying.moarvm
```

Для `copying.src` три параметра `--arg` задают размер полупространства,
ёмкость корней и число временных выделений. Они фиксируются при компиляции.
Программа выводит числовую статистику; `make demo` показывает её с подписями.

## Построение графиков

```sh
make setup-plots
make profile
```

Отдельные сценарии:

```sh
make profile-copying
make profile-animals
make profile-blackbox
```

Графики PNG/SVG и данные JSON сохраняются в
`lab3/output/figures/copying/`, `lab3/output/figures/animals/`
и `lab3/output/figures/blackbox/`.

Графики показывают занятость активного полупространства, число выживших
и скопированных объектов, количество обновлённых ссылок при сборках.

Для «чёрного ящика» можно изменить число операций, seed и размеры полупространств:

```sh
make profile-blackbox LAB3_BLACKBOX_STEPS=1000 LAB3_BLACKBOX_SEED=1 \
  LAB3_BLACKBOX_SPACES="128 256"
```

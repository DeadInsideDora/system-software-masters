# Лабораторная работа 4 — вариант 6

**Язык приложения — Perl, виртуальная машина — MoarVM.**

Консольное приложение на Perl вызывает функции, написанные на учебном языке
и скомпилированные в байткод MoarVM. Связь реализована через модуль
`SPO::MoarVM`, Perl XS и C-библиотеку `ffi/spo_vm.c`.
MoarVM выполняет функции внутри процесса Perl.

Приложение поддерживает три команды:

- `stats` — количество, сумма, минимум и максимум целых чисел;
- `scale` — умножение элементов массива на заданный множитель;
- `greet` — формирование приветствий для переданных имён.

Вычисления реализованы в `examples/lab4/operations.src`,
интерфейс приложения — в `examples/lab4/app.pl`.

## Требования

- GCC или Clang, Make, Flex, Bison;
- Perl с 64-битными целыми, заголовочными файлами и `ExtUtils::MakeMaker`;
- MoarVM **2026.08**, включая заголовочные файлы и библиотеку `libmoar`;
- curl, tar, shasum и доступ к интернету для установки MoarVM скриптом.

Команды выполняются из корня проекта в macOS или Linux.
На macOS для сборки нужны Xcode Command Line Tools.

## Сборка и запуск

```sh
git clone --branch lab4 git@github.com:DeadInsideDora/system-software-masters.git
cd system-software-masters
make setup-vm
make
make demo
```

`make setup-vm` собирает MoarVM 2026.08 и устанавливает её в `.local/moarvm`.
`make` собирает компилятор, модуль `SPO::MoarVM` и библиотеку функций
`output/lab4/operations.moarvm`.

Если MoarVM 2026.08 уже установлена с заголовочными файлами и `libmoar`,
вместо `make setup-vm` укажите каталог установки:

```sh
export MOARVM_PREFIX=/absolute/path/to/moarvm
export MOAR="$MOARVM_PREFIX/bin/moar"
make
make demo
```

Для выбора другого Perl используйте `make PERL=/path/to/perl`.
Запускайте приложение той же версией Perl, которой собирался XS-модуль.

## Команды приложения

```sh
perl examples/lab4/app.pl stats 7 -3 12 4
# Количество: 4; сумма: 20; минимум: -3; максимум: 12

perl examples/lab4/app.pl scale 3 7 -3 12 4
# 21 -9 36 12

perl examples/lab4/app.pl greet Мир Perl
# Привет, Мир!
# Привет, Perl!
```

Флаг `--json` перед командой включает вывод в JSON:

```sh
perl examples/lab4/app.pl --json stats 7 -3 12 4
# {"count":4,"max":12,"min":-3,"sum":20}
```

Справка:

```sh
perl examples/lab4/app.pl --help
```

## Компиляция библиотеки

После изменения `examples/lab4/operations.src` выполните `make`.
Для компиляции отдельного файла в библиотечном режиме:

```sh
mkdir -p output/lab4
build/moarvmdirect --library output/lab4/operations.moarvm \
  examples/lab4/operations.src
```

Флаг `--library` создаёт библиотеку экспортируемых функций без обязательной
функции `main()`. Модуль `SPO::MoarVM` поддерживает передачу целых чисел,
строк, массивов `int[]` и `string[]`, а также получение результатов вызова.

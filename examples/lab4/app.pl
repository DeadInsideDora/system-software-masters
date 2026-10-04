#!/usr/bin/env perl
use strict;
use warnings;
use utf8;
use FindBin qw($RealBin);
use lib "$RealBin/../../ffi/perl/blib/lib", "$RealBin/../../ffi/perl/blib/arch";
use Encode qw(decode FB_CROAK);
use JSON::PP;
use SPO::MoarVM;

binmode STDOUT, ':encoding(UTF-8)';
binmode STDERR, ':encoding(UTF-8)';
@ARGV = map { decode('UTF-8', $_, FB_CROAK) } @ARGV;
my $json = @ARGV && $ARGV[0] eq '--json' ? shift(@ARGV) : 0;
my $command = shift(@ARGV) // 'help';
if ($command eq 'help' || $command eq '--help') {
    print "Вычисления выполняет библиотека MoarVM, интерфейс — Perl.\n",
          "  app.pl [--json] stats [целые числа...]\n",
          "  app.pl [--json] scale множитель [целые числа...]\n",
          "  app.pl [--json] greet [имена...]\n";
    exit 0;
}
my $ok = eval {
    die "Неизвестная команда: $command\n" unless $command =~ /\A(?:stats|scale|greet)\z/;
    my $vm = SPO::MoarVM->new("$RealBin/../../output/lab4/operations.moarvm");
    my $result;
    if ($command eq 'stats') {
        my $values = $vm->call('stats', \@ARGV);
        $result = { count => $values->[0], sum => $values->[1],
                    min => $values->[0] ? $values->[2] : undef,
                    max => $values->[0] ? $values->[3] : undef };
        print "Количество: $result->{count}; сумма: $result->{sum}; минимум: ",
              ($result->{min} // '—'), "; максимум: ", ($result->{max} // '—'), "\n" unless $json;
    } elsif ($command eq 'scale') {
        die "Укажите целый множитель\n" unless @ARGV;
        my $factor = shift @ARGV;
        $result = $vm->call('scale', \@ARGV, $factor);
        print join(' ', @$result), "\n" unless $json;
    } else {
        $result = $vm->call('greet', \@ARGV);
        print "$_\n" for $json ? () : @$result;
    }
    print JSON::PP->new->canonical->encode($result), "\n" if $json;
    $vm->close;
    1;
};
if (!$ok) { print STDERR "Ошибка: $@"; exit 1; }

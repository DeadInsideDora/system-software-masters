use strict;
use warnings;
use Config;
use Cwd qw(abs_path);
use ExtUtils::MakeMaker;

my $root = abs_path('../..');
my $prefix = abs_path($ENV{MOARVM_PREFIX} || "$root/.local/moarvm")
    or die "MoarVM missing: run make setup-vm from the project root\n";
my $build = abs_path($ENV{SPO_BUILD_DIR} || "$root/build");
-f "$build/ffi_runtime.h" or die "Run make $build/ffi_runtime.h before building XS\n";
open my $version, '-|', "$prefix/bin/moar", '--version' or die "Cannot run MoarVM: $!";
my $version_text = do { local $/; <$version> };
close $version;
$version_text =~ /version 2026\.08\s*$/ or die "This bridge requires MoarVM 2026.08\n";
$Config{ivsize} == 8 or die "This bridge requires a Perl with 64-bit integers\n";
my @includes = ('..', $root, $build, map { "$prefix/include/$_" } qw(moar libuv dyncall libtommath mimalloc zmij));
if ($^O eq 'darwin' && !-f "$Config{archlib}/CORE/perl.h") {
    open my $sdk_pipe, '-|', 'xcrun', '--show-sdk-path' or die "Cannot locate macOS SDK: $!";
    chomp(my $sdk = <$sdk_pipe>); close $sdk_pipe;
    -f "$sdk$Config{archlib}/CORE/perl.h" or die "Install the Command Line Tools Perl headers\n";
    push @includes, "$sdk$Config{archlib}/CORE";
}
WriteMakefile(
    NAME => 'SPO::MoarVM',
    VERSION_FROM => 'lib/SPO/MoarVM.pm',
    XS => { 'MoarVM.xs' => 'MoarVM.c' },
    OBJECT => 'MoarVM$(OBJ_EXT) vm_bridge$(OBJ_EXT)',
    INC => join(' ', map { '-I"' . $_ . '"' } @includes),
    LIBS => ['-L"' . $prefix . '/lib" -lmoar'],
    LDDLFLAGS => $Config{lddlflags} . ' -Wl,-rpath,"' . $prefix . '/lib"',
    CCFLAGS => $Config{ccflags} . ' -Wall -Wextra -Wno-unused-parameter -Wno-unused-function',
    clean => { FILES => 'MoarVM.c' },
);
sub MY::postamble {
    return "\nvm_bridge\$(OBJ_EXT): ../spo_vm.c ../spo_vm.h ../../moarvm_ffi_abi.h $build/ffi_runtime.h\n";
}

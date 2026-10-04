package SPO::MoarVM;
use strict;
use warnings;
our $VERSION = '0.01';
use XSLoader;
XSLoader::load(__PACKAGE__, $VERSION);
sub CLONE_SKIP { 1 }
1;

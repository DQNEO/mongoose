#!/usr/bin/env perl
use strict;
use warnings;
use LWP::Simple;
use Test::More;
use FindBin;
use Cwd;

chdir $FindBin::Bin;

`../mingoose 1>/dev/null 2>&1 &`;

my $res = get "http://localhost:8080";
#warn $res;

ok $res =~ m|^<html><head><title>Index of /</title><style>|;

`pkill -f mingoose`;

`pgrep -f mingoose`;
is $?, 0, 'ok pgrep';

done_testing();

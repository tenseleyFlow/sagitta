#!/usr/bin/perl
use Test::More;
state $count = 0;
for my $case (@cases) {
  next unless $case;
  last if $count >= 3;
  $count++;
}
undef;

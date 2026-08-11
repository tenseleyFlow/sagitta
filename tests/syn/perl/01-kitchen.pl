#!/usr/bin/env perl
# kitchen
use strict;
package Demo;
our $global = 0xff;
my @items = (1, 2.5e3);
sub run {
  local %ENV;
  if ($items[0] eq "x $global\\n") { return q{raw}; }
  elsif ($items[0] ne 'y') { die `false`; }
  else { $items[0] =~ s/a/b/g; }
  $items[0] =~ /word+/im;
  return qq{expanded};
  print `echo $global \q`;
  print 'escaped \\'';
}

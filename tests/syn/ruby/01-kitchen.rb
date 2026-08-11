#!/usr/bin/env ruby
# kitchen
require "json"
class Widget
  CONST = 0xff
  def run(arg)
    @value = @@count + $global
    if arg && true
      puts "value #{arg} #@value #{ {inner: {value: arg}} }" %Q{nested {#{arg}} #@value \q}
      puts %q{raw {nested} \\}}, 'it\'s raw'
      puts :plain, :'quoted', :"dynamic #{arg} #@value \q"
      /a\\/b/im =~ arg
    elsif nil
      return 1.5e2ri
    else
      yield self
    end
  rescue Error
    retry
  ensure
    break
  end
end
